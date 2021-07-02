/*
 * Copyright (C) 2020-present ScyllaDB
 */

/*
 * This file is part of Scylla.
 *
 * Scylla is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Scylla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "server.hh"

#include <boost/range/adaptor/filtered.hpp>
#include <boost/range/adaptor/transformed.hpp>
#include <boost/range/adaptor/map.hpp>
#include <boost/range/algorithm/copy.hpp>
#include "boost/range/join.hpp"
#include <map>
#include <seastar/core/sleep.hh>
#include <seastar/core/future-util.hh>
#include <seastar/core/shared_future.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/pipe.hh>
#include <seastar/core/metrics.hh>
#include <absl/container/flat_hash_map.h>

#include "fsm.hh"
#include "log.hh"

using namespace std::chrono_literals;

namespace raft {

struct active_read {
    read_id id;
    index_t idx;
    seastar::promise<read_barrier_reply> promise;
};

static const seastar::metrics::label server_id_label("id");
static const seastar::metrics::label log_entry_type("log_entry_type");
static const seastar::metrics::label message_type("message_type");

class server_impl : public rpc_server, public server {
public:
    explicit server_impl(server_id uuid, std::unique_ptr<rpc> rpc,
        std::unique_ptr<state_machine> state_machine, std::unique_ptr<persistence> persistence,
        seastar::shared_ptr<failure_detector> failure_detector, server::configuration config);

    server_impl(server_impl&&) = delete;

    ~server_impl() {}

    // rpc_server interface
    void append_entries(server_id from, append_request append_request) override;
    void append_entries_reply(server_id from, append_reply reply) override;
    void request_vote(server_id from, vote_request vote_request) override;
    void request_vote_reply(server_id from, vote_reply vote_reply) override;
    void timeout_now_request(server_id from, timeout_now timeout_now) override;
    void read_quorum_request(server_id from, struct read_quorum read_quorum) override;
    void read_quorum_reply(server_id from, struct read_quorum_reply read_quorum_reply) override;
    future<read_barrier_reply> execute_read_barrier(server_id) override;


    // server interface
    future<> add_entry(command command, wait_type type) override;
    future<snapshot_reply> apply_snapshot(server_id from, install_snapshot snp) override;
    future<> set_configuration(server_address_set c_new) override;
    raft::configuration get_configuration() const override;
    future<> start() override;
    future<> abort() override;
    term_t get_current_term() const override;
    future<> read_barrier() override;
    void wait_until_candidate() override;
    future<> wait_election_done() override;
    future<> wait_log_idx_term(std::pair<index_t, term_t> idx_log) override;
    std::pair<index_t, term_t> log_last_idx_term() override;
    void elapse_election() override;
    bool is_leader() override;
    void tick() override;
    raft::server_id id() const override;
    future<> stepdown(logical_clock::duration timeout) override;
private:
    std::unique_ptr<rpc> _rpc;
    std::unique_ptr<state_machine> _state_machine;
    std::unique_ptr<persistence> _persistence;
    seastar::shared_ptr<failure_detector> _failure_detector;
    // Protocol deterministic finite-state machine
    std::unique_ptr<fsm> _fsm;
    // id of this server
    server_id _id;
    server::configuration _config;
    std::optional<promise<>> _stepdown_promise;
    std::optional<shared_promise<>> _leader_promise;
    // Index of the last entry applied to `_state_machine`.
    index_t _applied_idx;
    std::list<active_read> _reads;
    std::multimap<index_t, promise<>> _awaited_indexes;

    struct stop_apply_fiber{}; // exception to send when apply fiber is needs to be stopepd
    queue<std::variant<std::vector<log_entry_ptr>, snapshot_descriptor>> _apply_entries = queue<std::variant<std::vector<log_entry_ptr>, snapshot_descriptor>>(10);

    struct stats {
        uint64_t add_command = 0;
        uint64_t add_dummy = 0;
        uint64_t add_config = 0;
        uint64_t append_entries_received = 0;
        uint64_t append_entries_reply_received = 0;
        uint64_t request_vote_received = 0;
        uint64_t request_vote_reply_received = 0;
        uint64_t waiters_awaken = 0;
        uint64_t waiters_dropped = 0;
        uint64_t append_entries_reply_sent = 0;
        uint64_t append_entries_sent = 0;
        uint64_t vote_request_sent = 0;
        uint64_t vote_request_reply_sent = 0;
        uint64_t install_snapshot_sent = 0;
        uint64_t snapshot_reply_sent = 0;
        uint64_t polls = 0;
        uint64_t store_term_and_vote = 0;
        uint64_t store_snapshot = 0;
        uint64_t sm_load_snapshot = 0;
        uint64_t truncate_persisted_log = 0;
        uint64_t persisted_log_entries = 0;
        uint64_t queue_entries_for_apply = 0;
        uint64_t applied_entries = 0;
        uint64_t snapshots_taken = 0;
        uint64_t timeout_now_sent = 0;
        uint64_t timeout_now_received = 0;
        uint64_t read_quorum_sent = 0;
        uint64_t read_quorum_received = 0;
        uint64_t read_quorum_reply_sent = 0;
        uint64_t read_quorum_reply_received = 0;
    } _stats;

    struct op_status {
        term_t term; // term the entry was added with
        promise<> done; // notify when done here
    };

    // Entries that have a waiter that needs to be notified when the
    // respective entry is known to be committed.
    std::map<index_t, op_status> _awaited_commits;

    // Entries that have a waiter that needs to be notified after
    // the respective entry is applied.
    std::map<index_t, op_status> _awaited_applies;

    uint64_t _next_snapshot_transfer_id = 0;

    struct snapshot_transfer {
        future<> f;
        seastar::abort_source as;
        uint64_t id;
    };

    // Contains active snapshot transfers, to be waited on exit.
    std::unordered_map<server_id, snapshot_transfer> _snapshot_transfers;

    // Contains aborted snapshot transfers with still unresolved futures
    std::unordered_map<uint64_t, future<>> _aborted_snapshot_transfers;

    // The optional is engaged when incoming snapshot is received
    // And the promise signalled when it is successfully applied or there was an error
    std::unordered_map<server_id, promise<snapshot_reply>> _snapshot_application_done;

    struct append_request_queue {
        size_t count = 0;
        future<> f = make_ready_future<>();
    };
    absl::flat_hash_map<server_id, append_request_queue> _append_request_status;

    // Called to commit entries (on a leader or otherwise).
    void notify_waiters(std::map<index_t, op_status>& waiters, const std::vector<log_entry_ptr>& entries);

    // Drop waiter that we lost track of, can happen due to a snapshot transfer,
    // or a leader removed from cluster while some entries added on it are uncommitted.
    void drop_waiters(std::optional<index_t> idx = {});

    // Wake up all waiter that wait for entries with idx smaller of equal to the one provided
    // to be applied.
    void signal_applied();

    // This fiber processes FSM output by doing the following steps in order:
    //  - persist the current term and vote
    //  - persist unstable log entries on disk.
    //  - send out messages
    future<> io_fiber(index_t stable_idx);

    // This fiber runs in the background and applies committed entries.
    future<> applier_fiber();

    template <typename T> future<> add_entry_internal(T command, wait_type type);
    template <typename Message> void send_message(server_id id, Message m);

    // Abort all snapshot transfer.
    // Called when a server id is out of the configuration
    void abort_snapshot_transfer(server_id id);

    // Abort all snapshot transfers.
    // Called when no longer a leader or on shutdown
    void abort_snapshot_transfers();

    // Send snapshot in the background and notify FSM about the result.
    void send_snapshot(server_id id, install_snapshot&& snp);

    future<> _applier_status = make_ready_future<>();
    future<> _io_status = make_ready_future<>();

    void register_metrics();
    seastar::metrics::metric_groups _metrics;

    // Server address set to be used by RPC module to maintain its address
    // mappings.
    // Doesn't really correspond to any configuration, neither
    // committed, nor applied. This is just an artificial address set
    // meant entirely for RPC purposes and is constructed from the last
    // configuration entry in the log (prior to sending out the messages in the
    // `io_fiber`) as follows:
    // * If the config is non-joint, it's the current configuration.
    // * If the config is joint, it's defined as a union of current and
    //   previous configurations.
    //   The motivation behind this is that server should have a collective
    //   set of addresses from both leaving and joining nodes before
    //   sending the messages, because it may send to both types of nodes.
    // After the new address set is built the diff between the last rpc config
    // observed by the `server_impl` instance and the one obtained from the last
    // conf entry is calculated. The diff is used to maintain rpc state for
    // joining and leaving servers.
    server_address_set _current_rpc_config;
    const server_address_set& get_rpc_config() const;
    // Per-item updates to rpc config.
    void add_to_rpc_config(server_address srv);
    void remove_from_rpc_config(const server_address& srv);

    // A helper to wait for a leader to get elected
    future<> wait_for_leader();

    // Get "safe to read" index from a leader
    future<read_barrier_reply> get_read_idx(server_id leader);
    // Wait for the index to be applied
    future<> wait_for_apply(index_t idx);

    friend std::ostream& operator<<(std::ostream& os, const server_impl& s);
};

server_impl::server_impl(server_id uuid, std::unique_ptr<rpc> rpc,
        std::unique_ptr<state_machine> state_machine, std::unique_ptr<persistence> persistence,
        seastar::shared_ptr<failure_detector> failure_detector, server::configuration config) :
                    _rpc(std::move(rpc)), _state_machine(std::move(state_machine)),
                    _persistence(std::move(persistence)), _failure_detector(failure_detector),
                    _id(uuid), _config(config) {
    set_rpc_server(_rpc.get());
    if (_config.snapshot_threshold > _config.max_log_size) {
        throw config_error("snapshot_threshold has to be smaller than max_log_size");
    }
}

future<> server_impl::start() {
    auto [term, vote] = co_await _persistence->load_term_and_vote();
    auto snapshot  = co_await _persistence->load_snapshot_descriptor();
    auto log_entries = co_await _persistence->load_log();
    auto log = raft::log(snapshot, std::move(log_entries));
    raft::configuration rpc_config = log.get_configuration();
    index_t stable_idx = log.stable_idx();
    _fsm = std::make_unique<fsm>(_id, term, vote, std::move(log), *_failure_detector,
                                 fsm_config {
                                     .append_request_threshold = _config.append_request_threshold,
                                     .max_log_size = _config.max_log_size,
                                     .enable_prevoting = _config.enable_prevoting
                                 });

    _applied_idx = index_t{0};
    if (snapshot.id) {
        co_await _state_machine->load_snapshot(snapshot.id);
        _applied_idx = snapshot.idx;
    }

    if (!rpc_config.current.empty()) {
        // Update RPC address map from the latest configuration (either from
        // the log or the snapshot)
        //
        // Account both for current and previous configurations since
        // the last configuration idx can point to the joint configuration entry.
        rpc_config.current.merge(rpc_config.previous);
        for (const auto& addr: rpc_config.current) {
            add_to_rpc_config(addr);
            _rpc->add_server(addr.id, addr.info);
        }
    }

    // start fiber to persist entries added to in-memory log
    _io_status = io_fiber(stable_idx);
    // start fiber to apply committed entries
    _applier_status = applier_fiber();

    // Metrics access _fsm, so create them only after the pointer is populated
    register_metrics();
    co_return;
}

future<> server_impl::wait_for_leader() {
    if (!_leader_promise) {
        _leader_promise.emplace();
    }
    return _leader_promise->get_shared_future();
}

template <typename T>
future<> server_impl::add_entry_internal(T command, wait_type type) {
    logger.trace("An entry is submitted on a leader");

    // Wait for a new slot to become available
    co_await _fsm->wait_max_log_size();

    logger.trace("An entry proceeds after wait");

    const log_entry& e = _fsm->add_entry(std::move(command));

    auto& container = type == wait_type::committed ? _awaited_commits : _awaited_applies;

    // This will track the commit/apply status of the entry
    auto [it, inserted] = container.emplace(e.idx, op_status{e.term, promise<>()});
    assert(inserted);
    co_return co_await it->second.done.get_future();
}

future<> server_impl::add_entry(command command, wait_type type) {
    _stats.add_command++;
    return add_entry_internal(std::move(command), type);
}

void server_impl::append_entries(server_id from, append_request append_request) {
    _stats.append_entries_received++;
    _fsm->step(from, std::move(append_request));
}

void server_impl::append_entries_reply(server_id from, append_reply reply) {
    _stats.append_entries_reply_received++;
    _fsm->step(from, std::move(reply));
}

void server_impl::request_vote(server_id from, vote_request vote_request) {
    _stats.request_vote_received++;
    _fsm->step(from, std::move(vote_request));
}

void server_impl::request_vote_reply(server_id from, vote_reply vote_reply) {
    _stats.request_vote_reply_received++;
    _fsm->step(from, std::move(vote_reply));
}

void server_impl::timeout_now_request(server_id from, timeout_now timeout_now) {
    _stats.timeout_now_received++;
    _fsm->step(from, std::move(timeout_now));
}

void server_impl::read_quorum_request(server_id from, struct read_quorum read_quorum) {
    _stats.read_quorum_received++;
    _fsm->step(from, std::move(read_quorum));
}

void server_impl::read_quorum_reply(server_id from, struct read_quorum_reply read_quorum_reply) {
    _stats.read_quorum_reply_received++;
    _fsm->step(from, std::move(read_quorum_reply));
}

void server_impl::notify_waiters(std::map<index_t, op_status>& waiters,
        const std::vector<log_entry_ptr>& entries) {
    index_t commit_idx = entries.back()->idx;
    index_t first_idx = entries.front()->idx;

    while (waiters.size() != 0) {
        auto it = waiters.begin();
        if (it->first > commit_idx) {
            break;
        }
        auto [entry_idx, status] = std::move(*it);

        // if there is a waiter entry with an index smaller than first entry
        // it means that notification is out of order which is prohibited
        assert(entry_idx >= first_idx);

        waiters.erase(it);
        if (status.term == entries[entry_idx - first_idx]->term) {
            status.done.set_value();
        } else {
            // The terms do not match which means that between the
            // times the entry was submitted and committed there
            // was a leadership change and the entry was replaced.
            status.done.set_exception(dropped_entry());
        }
        _stats.waiters_awaken++;
    }
    // Drop all waiters with smaller term that last one been committed
    // since there is no way they will be committed any longer (terms in
    // the log only grow).
    term_t last_committed_term = entries.back()->term;
    while (waiters.size() != 0) {
        auto it = waiters.begin();
        if (it->second.term < last_committed_term) {
            it->second.done.set_exception(dropped_entry());
            waiters.erase(it);
            _stats.waiters_awaken++;
        } else {
            break;
        }
    }
}

void server_impl::drop_waiters(std::optional<index_t> idx) {
    auto drop = [&] (std::map<index_t, op_status>& waiters) {
        while (waiters.size() != 0) {
            auto it = waiters.begin();
            if (idx && it->first > *idx) {
                break;
            }
            auto [entry_idx, status] = std::move(*it);
            waiters.erase(it);
            status.done.set_exception(commit_status_unknown());
            _stats.waiters_dropped++;
        }
    };
    drop(_awaited_commits);
    drop(_awaited_applies);
}

void server_impl::signal_applied() {
    auto it = _awaited_indexes.begin();

    while (it != _awaited_indexes.end()) {
        if (it->first > _applied_idx) {
            break;
        }
        it->second.set_value();
        it = _awaited_indexes.erase(it);
    }
}

template <typename Message>
void server_impl::send_message(server_id id, Message m) {
    std::visit([this, id] (auto&& m) {
        using T = std::decay_t<decltype(m)>;
        if constexpr (std::is_same_v<T, append_reply>) {
            _stats.append_entries_reply_sent++;
            _rpc->send_append_entries_reply(id, m);
        } else if constexpr (std::is_same_v<T, append_request>) {
            _stats.append_entries_sent++;
             _append_request_status[id].count++;
             _append_request_status[id].f = _append_request_status[id].f.then([this, cm = std::move(m), cid = id] () noexcept -> future<> {
                // We need to copy everything from the capture because it cannot be accessed after co-routine yields.
                server_impl* server = this;
                auto m = std::move(cm);
                auto id = cid;
                try {
                    co_await server->_rpc->send_append_entries(id, m);
                } catch(...) {
                    logger.debug("[{}] io_fiber failed to send a message to {}: {}", server->_id, id, std::current_exception());
                }
                server->_append_request_status[id].count--;
                if (server->_append_request_status[id].count == 0) {
                   server->_append_request_status.erase(id);
                }
            });
        } else if constexpr (std::is_same_v<T, vote_request>) {
            _stats.vote_request_sent++;
            _rpc->send_vote_request(id, m);
        } else if constexpr (std::is_same_v<T, vote_reply>) {
            _stats.vote_request_reply_sent++;
            _rpc->send_vote_reply(id, m);
        } else if constexpr (std::is_same_v<T, timeout_now>) {
            _stats.timeout_now_sent++;
            _rpc->send_timeout_now(id, m);
        } else if constexpr (std::is_same_v<T, struct read_quorum>) {
            _stats.read_quorum_sent++;
            _rpc->send_read_quorum(id, std::move(m));
        } else if constexpr (std::is_same_v<T, struct read_quorum_reply>) {
            _stats.read_quorum_reply_sent++;
            _rpc->send_read_quorum_reply(id, std::move(m));
        } else if constexpr (std::is_same_v<T, install_snapshot>) {
            _stats.install_snapshot_sent++;
            // Send in the background.
            send_snapshot(id, std::move(m));
        } else if constexpr (std::is_same_v<T, snapshot_reply>) {
            _stats.snapshot_reply_sent++;
            assert(_snapshot_application_done.contains(id));
            // Send a reply to install_snapshot after
            // snapshot application is done.
            _snapshot_application_done[id].set_value(std::move(m));
            _snapshot_application_done.erase(id);
        } else {
            static_assert(!sizeof(T*), "not all message types are handled");
        }
    }, std::move(m));
}

static configuration_diff diff_address_sets(const server_address_set& prev, const server_address_set& current) {
    configuration_diff result;
    for (const auto& s : current) {
        if (!prev.contains(s)) {
            result.joining.insert(s);
        }
    }
    for (const auto& s : prev) {
        if (!current.contains(s)) {
            result.leaving.insert(s);
        }
    }
    return result;
}

future<> server_impl::io_fiber(index_t last_stable) {
    logger.trace("[{}] io_fiber start", _id);
    try {
        while (true) {
            auto batch = co_await _fsm->poll_output();
            _stats.polls++;

            if (batch.term_and_vote) {
                // Current term and vote are always persisted
                // together. A vote may change independently of
                // term, but it's safe to update both in this
                // case.
                co_await _persistence->store_term_and_vote(batch.term_and_vote->first, batch.term_and_vote->second);
                _stats.store_term_and_vote++;
            }

            if (batch.snp) {
                auto& [snp, is_local, old_id] = *batch.snp;
                logger.trace("[{}] io_fiber storing snapshot {}", _id, snp.id);
                // Persist the snapshot
                co_await _persistence->store_snapshot_descriptor(snp, is_local ? _config.snapshot_trailing : 0);
                _stats.store_snapshot++;
                // Drop previous snapshot since it is no longer used
                 _state_machine->drop_snapshot(old_id);
                // If this is locally generated snapshot there is no need to
                // load it.
                if (!is_local) {
                    co_await _apply_entries.push_eventually(std::move(snp));
                }
            }

            if (batch.log_entries.size()) {
                auto& entries = batch.log_entries;

                if (last_stable >= entries[0]->idx) {
                    co_await _persistence->truncate_log(entries[0]->idx);
                    _stats.truncate_persisted_log++;
                }

                // Combine saving and truncating into one call?
                // will require persistence to keep track of last idx
                co_await _persistence->store_log_entries(entries);

                last_stable = (*entries.crbegin())->idx;
                _stats.persisted_log_entries += entries.size();
            }

            // Update RPC server address mappings. Add servers which are joining
            // the cluster according to the new configuration (obtained from the
            // last_conf_idx).
            //
            // It should be done prior to sending the messages since the RPC
            // module needs to know who should it send the messages to (actual
            // network addresses of the joining servers).
            configuration_diff rpc_diff;
            if (batch.configuration) {
                const server_address_set& current_rpc_config = get_rpc_config();
                rpc_diff = diff_address_sets(get_rpc_config(), *batch.configuration);
                for (const auto& addr: rpc_diff.joining) {
                    add_to_rpc_config(addr);
                    _rpc->add_server(addr.id, addr.info);
                }
            }

             // After entries are persisted we can send messages.
            for (auto&& m : batch.messages) {
                try {
                    send_message(m.first, std::move(m.second));
                } catch(...) {
                    // Not being able to send a message is not a critical error
                    logger.debug("[{}] io_fiber failed to send a message to {}: {}", _id, m.first, std::current_exception());
                }
            }

            if (batch.configuration) {
                for (const auto& addr: rpc_diff.leaving) {
                    abort_snapshot_transfer(addr.id);
                    remove_from_rpc_config(addr);
                    _rpc->remove_server(addr.id);
                }
            }

            // Process committed entries.
            if (batch.committed.size()) {
                _stats.queue_entries_for_apply += batch.committed.size();
                co_await _apply_entries.push_eventually(std::move(batch.committed));
            }

            if (batch.max_read_id_with_quorum) {
                while (!_reads.empty() && _reads.front().id <= batch.max_read_id_with_quorum) {
                    _reads.front().promise.set_value(_reads.front().idx);
                    _reads.pop_front();
                }
            }
            if (!_fsm->is_leader()) {
                if (_stepdown_promise) {
                    std::exchange(_stepdown_promise, std::nullopt)->set_value();
                }
                if (!_current_rpc_config.contains(server_address{_id})) {
                    // If the node is no longer part of a config and no longer the leader
                    // it will never know the status of entries it submitted
                    drop_waiters();
                }
                // request aborts of snapshot transfers
                abort_snapshot_transfers();
                // abort all read barriers
                for (auto& r : _reads) {
                    r.promise.set_value(not_a_leader{_fsm->current_leader()});
                }
                _reads.clear();
            } else if (batch.abort_leadership_transfer) {
                if (_stepdown_promise) {
                    std::exchange(_stepdown_promise, std::nullopt)->set_exception(timeout_error("Stepdown process timed out"));
                }
            }
            if (_leader_promise && _fsm->current_leader()) {
                std::exchange(_leader_promise, std::nullopt)->set_value();
            }
        }
    } catch (seastar::broken_condition_variable&) {
        // Log fiber is stopped explicitly.
    } catch (stop_apply_fiber&) {
        // Log fiber is stopped explicitly
    } catch (...) {
        logger.error("[{}] io fiber stopped because of the error: {}", _id, std::current_exception());
    }
    co_return;
}

void server_impl::send_snapshot(server_id dst, install_snapshot&& snp) {
    seastar::abort_source as;
    uint64_t id = _next_snapshot_transfer_id++;
    // Use `later()` to ensure that `_rpc->send_snapshot` is called after we emplace `f` in `_snapshot_transfers`.
    // This also catches any exceptions from `_rpc->send_snapshot` into `f`.
    future<> f = later().then([this, &as, dst, id, snp = std::move(snp)] () mutable {
        return _rpc->send_snapshot(dst, std::move(snp), as).then_wrapped([this, dst, id] (future<snapshot_reply> f) {
            if (_aborted_snapshot_transfers.erase(id)) {
                // The transfer was aborted
                f.ignore_ready_future();
                return;
            }
            _snapshot_transfers.erase(dst);
            auto reply = raft::snapshot_reply{.current_term = _fsm->get_current_term(), .success = false};
            if (f.failed()) {
                logger.error("[{}] Transferring snapshot to {} failed with: {}", _id, dst, f.get_exception());
            } else {
                logger.trace("[{}] Transferred snapshot to {}", _id, dst);
                reply = f.get();
            }
            _fsm->step(dst, std::move(reply));
        });
    });
    auto res = _snapshot_transfers.emplace(dst, snapshot_transfer{std::move(f), std::move(as), id});
    assert(res.second);
}

future<snapshot_reply> server_impl::apply_snapshot(server_id from, install_snapshot snp) {
    _fsm->step(from, std::move(snp));
    // Only one snapshot can be received at a time from each node
    assert(! _snapshot_application_done.contains(from));
    snapshot_reply reply{_fsm->get_current_term(), false};
    try {
        reply = co_await _snapshot_application_done[from].get_future();
    } catch (...) {
        logger.error("apply_snapshot[{}] failed with {}", _id, std::current_exception());
    }
    if (!reply.success) {
        // Drop snapshot that failed to be applied
        _state_machine->drop_snapshot(snp.snp.id);
    }

    co_return reply;
}

future<> server_impl::applier_fiber() {
    logger.trace("applier_fiber start");

    try {
        while (true) {
            auto v = co_await _apply_entries.pop_eventually();

            if (std::holds_alternative<std::vector<log_entry_ptr>>(v)) {
                auto& batch = std::get<0>(v);
                if (batch.empty()) {
                    logger.trace("[{}] applier fiber: received empty batch", _id);
                    continue;
                }

                // Completion notification code assumes that previous snapshot is applied
                // before new entries are committed, otherwise it asserts that some
                // notifications were missing. To prevent a committed entry to
                // be notified before an erlier snapshot is applied do both
                // notification and snapshot application in the same fiber
                notify_waiters(_awaited_commits, batch);

                std::vector<command_cref> commands;
                commands.reserve(batch.size());

                index_t last_idx = batch.back()->idx;
                term_t last_term = batch.back()->term;
                assert(last_idx == _applied_idx + batch.size());

                boost::range::copy(
                       batch |
                       boost::adaptors::filtered([] (log_entry_ptr& entry) { return std::holds_alternative<command>(entry->data); }) |
                       boost::adaptors::transformed([] (log_entry_ptr& entry) { return std::cref(std::get<command>(entry->data)); }),
                       std::back_inserter(commands));

                auto size = commands.size();
                if (size) {
                    co_await _state_machine->apply(std::move(commands));
                    _stats.applied_entries += size;
                }

               _applied_idx = last_idx;
               notify_waiters(_awaited_applies, batch);

               // It may happen that _fsm has already applied a later snapshot (from remote) that we didn't yet 'observe'
               // (i.e. didn't yet receive from _apply_entries queue) but will soon. We avoid unnecessary work
               // of taking snapshots ourselves but comparing our last index directly with what's currently in _fsm.
               auto last_snap_idx = _fsm->log_last_snapshot_idx();
               if (_applied_idx >= last_snap_idx && _applied_idx - last_snap_idx >= _config.snapshot_threshold) {
                   snapshot_descriptor snp;
                   snp.term = last_term;
                   snp.idx = _applied_idx;
                   snp.config = _fsm->log_last_conf_for(_applied_idx);
                   logger.trace("[{}] applier fiber: taking snapshot term={}, idx={}", _id, snp.term, snp.idx);
                   snp.id = co_await _state_machine->take_snapshot();
                   // Note that at this point (after the `co_await`), _fsm may already have applied a later snapshot.
                   // That's fine, `_fsm->apply_snapshot` will simply ignore our current attempt; we will soon receive
                   // a later snapshot from the queue.
                   if (!_fsm->apply_snapshot(snp, _config.snapshot_trailing, true)) {
                       logger.trace("[{}] applier fiber: while taking snapshot term={} idx={} id={},"
                              " fsm received a later snapshot at idx={}", _id, snp.term, snp.idx, snp.id, _fsm->log_last_snapshot_idx());
                       _state_machine->drop_snapshot(snp.id);
                   }
                   _stats.snapshots_taken++;
               }
            } else {
                snapshot_descriptor& snp = std::get<1>(v);
                assert(snp.idx >= _applied_idx);
                // Apply snapshot it to the state machine
                logger.trace("[{}] apply_fiber applying snapshot {}", _id, snp.id);
                co_await _state_machine->load_snapshot(snp.id);
                drop_waiters(snp.idx);
                _applied_idx = snp.idx;
                _stats.sm_load_snapshot++;
            }
            signal_applied();
        }
    } catch(stop_apply_fiber& ex) {
        // the fiber is aborted
    } catch (...) {
        logger.error("[{}] applier fiber stopped because of the error: {}", _id, std::current_exception());
    }
    co_return;
}

term_t server_impl::get_current_term() const {
    return _fsm->get_current_term();
}

future<> server_impl::wait_for_apply(index_t idx) {
    if (idx > _applied_idx) {
        // The index is not applied yet. Wait for it.
        // This will be signalled when read_idx is applied
        auto it = _awaited_indexes.emplace(idx, promise<>());
        co_await it->second.get_future();
    }
}

future<read_barrier_reply> server_impl::execute_read_barrier(server_id from) {
    logger.trace("[{}] execute_read_barrier start", _id);

    std::optional<std::pair<read_id, index_t>> rid;
    try {
        rid = _fsm->start_read_barrier(from);
        if (!rid) {
            // cannot start a barrier yet
            return make_ready_future<read_barrier_reply>(std::monostate{});
        }
    } catch (not_a_leader& err) {
        return make_ready_future<read_barrier_reply>(err);
    }
    logger.trace("[{}] execute_read_barrier read id is {} for commit idx {}",
        _id, rid->first, rid->second);
    _reads.push_back({rid->first, rid->second, {}});
    return _reads.back().promise.get_future();
}

future<read_barrier_reply> server_impl::get_read_idx(server_id leader) {
    if (_id == leader) {
        return execute_read_barrier(_id);
    } else {
        return _rpc->execute_read_barrier_on_leader(leader);
    }
}

future<> server_impl::read_barrier() {
    server_id leader = _fsm->current_leader();

    logger.trace("[{}] read_barrier start", _id);
    index_t read_idx;

    while (read_idx == index_t{}) {
        logger.trace("[{}] read_barrier forward to  {}", _id, leader);
        if (leader == server_id{}) {
            co_await wait_for_leader();
            leader = _fsm->current_leader();
        } else {
            auto applied = _applied_idx;
            auto res = co_await get_read_idx(leader);
            if (std::holds_alternative<std::monostate>(res)) {
                // the leader is not ready to answer because it did not
                // committed any entries yet, so wait for any entry to be
                // committed (if non were since start of the attempt) and retry.
                logger.trace("[{}] read_barrier leader not ready", _id);
                co_await wait_for_apply(++applied);
            } else if (std::holds_alternative<raft::not_a_leader>(res)) {
                leader = std::get<not_a_leader>(res).leader;
            } else {
                read_idx = std::get<index_t>(res);
            }
        }
    }

    logger.trace("[{}] read_barrier read index {}, append index {}", _id, read_idx, _applied_idx);
    co_return co_await wait_for_apply(read_idx);
}

void server_impl::abort_snapshot_transfer(server_id id) {
    auto it = _snapshot_transfers.find(id);
    if (it != _snapshot_transfers.end()) {
        auto& [f, as, tid] = it->second;
        logger.trace("[{}] Request abort of snapshot transfer to {}", _id, id);
        as.request_abort();
        _aborted_snapshot_transfers.emplace(tid, std::move(f));
        _snapshot_transfers.erase(it);
    }
}

void server_impl::abort_snapshot_transfers() {
    for (auto&& [id, t] : _snapshot_transfers) {
        logger.trace("[{}] Request abort of snapshot transfer to {}", _id, id);
        t.as.request_abort();
        _aborted_snapshot_transfers.emplace(t.id, std::move(t.f));
    }
    _snapshot_transfers.clear();
}

future<> server_impl::abort() {
    logger.trace("abort() called");
    _fsm->stop();
    _apply_entries.abort(std::make_exception_ptr(stop_apply_fiber()));

    // IO and applier fibers may update waiters and start new snapshot
    // transfers, so abort() them first
    co_await seastar::when_all_succeed(std::move(_io_status), std::move(_applier_status),
                        _rpc->abort(), _state_machine->abort(), _persistence->abort()).discard_result();

    for (auto& ac: _awaited_commits) {
        ac.second.done.set_exception(stopped_error());
    }
    for (auto& aa: _awaited_applies) {
        aa.second.done.set_exception(stopped_error());
    }
    _awaited_commits.clear();
    _awaited_applies.clear();
    if (_leader_promise) {
        _leader_promise->set_exception(stopped_error());
    }

    // Complete all read attempts with not_a_leader
    for (auto& r: _reads) {
        r.promise.set_value(raft::not_a_leader{server_id{}});
    }
    _reads.clear();

    // Abort all read_barriers with an exception
    for (auto& i : _awaited_indexes) {
        i.second.set_exception(stopped_error());
    }
    _awaited_indexes.clear();

    for (auto&& [_, f] : _snapshot_application_done) {
        f.set_exception(std::runtime_error("Snapshot application aborted"));
    }

    abort_snapshot_transfers();

    auto snp_futures = _aborted_snapshot_transfers | boost::adaptors::map_values;

    auto append_futures = _append_request_status | boost::adaptors::map_values |  boost::adaptors::transformed([] (append_request_queue& a) -> future<>& { return a.f; });

    auto all_futures = boost::range::join(snp_futures, append_futures);

    co_await seastar::when_all_succeed(all_futures.begin(), all_futures.end()).discard_result();
}

future<> server_impl::set_configuration(server_address_set c_new) {
    const auto& cfg = _fsm->get_configuration();
    // 4.1 Cluster membership changes. Safety.
    // When the leader receives a request to add or remove a server
    // from its current configuration (C old ), it appends the new
    // configuration (C new ) as an entry in its log and replicates
    // that entry using the normal Raft mechanism.
    auto [joining, leaving] = cfg.diff(c_new);
    if (joining.size() == 0 && leaving.size() == 0) {
        co_return;
    }
    _stats.add_config++;
    co_await add_entry_internal(raft::configuration{std::move(c_new)}, wait_type::committed);
     // Above we co_wait that the joint configuration is committed.
     // Immediately, without yield, once the FSM discovers
     // this, it appends non-joint entry. Hence,
     // at this point in execution, non-joint entry is in the log.
     // By waiting for a follow up dummy to get committed, we
     // automatically wait for the non-joint entry to get
     // committed.
    co_await add_entry_internal(log_entry::dummy(), wait_type::committed);
}

raft::configuration
server_impl::get_configuration() const {
    return _fsm->get_configuration();
}

void server_impl::register_metrics() {
    namespace sm = seastar::metrics;
    _metrics.add_group("raft", {
        sm::make_total_operations("add_entries", _stats.add_command,
             sm::description("how many entries were added on this node"), {server_id_label(_id), log_entry_type("command")}),
        sm::make_total_operations("add_entries", _stats.add_dummy,
             sm::description("how many entries were added on this node"), {server_id_label(_id), log_entry_type("dummy")}),
        sm::make_total_operations("add_entries", _stats.add_config,
             sm::description("how many entries were added on this node"), {server_id_label(_id), log_entry_type("config")}),

        sm::make_total_operations("messages_received", _stats.append_entries_received,
             sm::description("how many messages were received"), {server_id_label(_id), message_type("append_entries")}),
        sm::make_total_operations("messages_received", _stats.append_entries_reply_received,
             sm::description("how many messages were received"), {server_id_label(_id), message_type("append_entries_reply")}),
        sm::make_total_operations("messages_received", _stats.request_vote_received,
             sm::description("how many messages were received"), {server_id_label(_id), message_type("request_vote")}),
        sm::make_total_operations("messages_received", _stats.request_vote_reply_received,
             sm::description("how many messages were received"), {server_id_label(_id), message_type("request_vote_reply")}),
        sm::make_total_operations("messages_received", _stats.timeout_now_received,
             sm::description("how many messages were received"), {server_id_label(_id), message_type("timeout_now")}),
        sm::make_total_operations("messages_received", _stats.read_quorum_received,
             sm::description("how many messages were received"), {server_id_label(_id), message_type("read_quorum")}),
        sm::make_total_operations("messages_received", _stats.read_quorum_reply_received,
             sm::description("how many messages were received"), {server_id_label(_id), message_type("read_quorum_reply")}),

        sm::make_total_operations("messages_sent", _stats.append_entries_sent,
             sm::description("how many messages were send"), {server_id_label(_id), message_type("append_entries")}),
        sm::make_total_operations("messages_sent", _stats.append_entries_reply_sent,
             sm::description("how many messages were sent"), {server_id_label(_id), message_type("append_entries_reply")}),
        sm::make_total_operations("messages_sent", _stats.vote_request_sent,
             sm::description("how many messages were sent"), {server_id_label(_id), message_type("request_vote")}),
        sm::make_total_operations("messages_sent", _stats.vote_request_reply_sent,
             sm::description("how many messages were sent"), {server_id_label(_id), message_type("request_vote_reply")}),
        sm::make_total_operations("messages_sent", _stats.install_snapshot_sent,
             sm::description("how many messages were sent"), {server_id_label(_id), message_type("install_snapshot")}),
        sm::make_total_operations("messages_sent", _stats.snapshot_reply_sent,
             sm::description("how many messages were sent"), {server_id_label(_id), message_type("snapshot_reply")}),
        sm::make_total_operations("messages_sent", _stats.timeout_now_sent,
             sm::description("how many messages were sent"), {server_id_label(_id), message_type("timeout_now")}),
        sm::make_total_operations("messages_sent", _stats.read_quorum_sent,
             sm::description("how many messages were sent"), {server_id_label(_id), message_type("read_quorum")}),
        sm::make_total_operations("messages_sent", _stats.read_quorum_reply_sent,
             sm::description("how many messages were sent"), {server_id_label(_id), message_type("read_quorum_reply")}),

        sm::make_total_operations("waiter_awaken", _stats.waiters_awaken,
             sm::description("how many waiters got result back"), {server_id_label(_id)}),
        sm::make_total_operations("waiter_dropped", _stats.waiters_dropped,
             sm::description("how many waiters did not get result back"), {server_id_label(_id)}),
        sm::make_total_operations("polls", _stats.polls,
             sm::description("how many time raft state machine was polled"), {server_id_label(_id)}),
        sm::make_total_operations("store_term_and_vote", _stats.store_term_and_vote,
             sm::description("how many times term and vote were persisted"), {server_id_label(_id)}),
        sm::make_total_operations("store_snapshot", _stats.store_snapshot,
             sm::description("how many snapshot were persisted"), {server_id_label(_id)}),
        sm::make_total_operations("sm_load_snapshot", _stats.sm_load_snapshot,
             sm::description("how many times user state machine was reloaded with a snapshot"), {server_id_label(_id)}),
        sm::make_total_operations("truncate_persisted_log", _stats.truncate_persisted_log,
             sm::description("how many times log was truncated on storage"), {server_id_label(_id)}),
        sm::make_total_operations("persisted_log_entries", _stats.persisted_log_entries,
             sm::description("how many log entries were persisted"), {server_id_label(_id)}),
        sm::make_total_operations("queue_entries_for_apply", _stats.queue_entries_for_apply,
             sm::description("how many log entries were queued to be applied"), {server_id_label(_id)}),
        sm::make_total_operations("applied_entries", _stats.applied_entries,
             sm::description("how many log entries were applied"), {server_id_label(_id)}),
        sm::make_total_operations("snapshots_taken", _stats.snapshots_taken,
             sm::description("how many time the user's state machine was snapshotted"), {server_id_label(_id)}),

        sm::make_gauge("in_memory_log_size", [this] { return _fsm->in_memory_log_size(); },
             sm::description("size of in-memory part of the log"), {server_id_label(_id)}),
    });
}

void server_impl::wait_until_candidate() {
    while (_fsm->is_follower()) {
        _fsm->tick();
    }
}

// Wait until candidate is either leader or reverts to follower
future<> server_impl::wait_election_done() {
    while (_fsm->is_candidate()) {
        co_await later();
    };
}

future<> server_impl::wait_log_idx_term(std::pair<index_t, term_t> idx_log) {
    while (_fsm->log_last_term() < idx_log.second || _fsm->log_last_idx() < idx_log.first) {
        co_await seastar::sleep(5us);
    }
}

std::pair<index_t, term_t> server_impl::log_last_idx_term() {
    return {_fsm->log_last_idx(), _fsm->log_last_term()};
}

bool server_impl::is_leader() {
    return _fsm->is_leader();
}

void server_impl::elapse_election() {
    while (_fsm->election_elapsed() < ELECTION_TIMEOUT) {
        _fsm->tick();
    }
}

void server_impl::tick() {
    _fsm->tick();
}

raft::server_id server_impl::id() const {
    return _id;
}

const server_address_set& server_impl::get_rpc_config() const {
    return _current_rpc_config;
}

void server_impl::add_to_rpc_config(server_address srv) {
    _current_rpc_config.emplace(std::move(srv));
}

void server_impl::remove_from_rpc_config(const server_address& srv) {
    _current_rpc_config.erase(srv);
}

future<> server_impl::stepdown(logical_clock::duration timeout) {
    if (_stepdown_promise) {
        return make_exception_future<>(std::logic_error("Stepdown is already in progress"));
    }
    try {
        _fsm->transfer_leadership(timeout);
    } catch (...) {
        return make_exception_future<>(std::current_exception());
    }
    _stepdown_promise = promise<>();
    return _stepdown_promise->get_future();
}

std::unique_ptr<server> create_server(server_id uuid, std::unique_ptr<rpc> rpc,
    std::unique_ptr<state_machine> state_machine, std::unique_ptr<persistence> persistence,
    seastar::shared_ptr<failure_detector> failure_detector, server::configuration config) {
    assert(uuid != raft::server_id{utils::UUID(0, 0)});
    return std::make_unique<raft::server_impl>(uuid, std::move(rpc), std::move(state_machine),
        std::move(persistence), failure_detector, config);
}

std::ostream& operator<<(std::ostream& os, const server_impl& s) {
    os << "[id: " << s._id << ", fsm (" << s._fsm << ")]\n";
    return os;
}

} // end of namespace raft
