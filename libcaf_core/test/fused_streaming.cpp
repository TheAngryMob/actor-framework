/******************************************************************************
 *                       ____    _    _____                                   *
 *                      / ___|  / \  |  ___|    C++                           *
 *                     | |     / _ \ | |_       Actor                         *
 *                     | |___ / ___ \|  _|      Framework                     *
 *                      \____/_/   \_|_|                                      *
 *                                                                            *
 * Copyright 2011-2018 Dominik Charousset                                     *
 *                                                                            *
 * Distributed under the terms and conditions of the BSD 3-Clause License or  *
 * (at your option) under the terms and conditions of the Boost Software      *
 * License 1.0. See accompanying files LICENSE and LICENSE_ALTERNATIVE.       *
 *                                                                            *
 * If you did not receive a copy of the license files, see                    *
 * http://opensource.org/licenses/BSD-3-Clause and                            *
 * http://www.boost.org/LICENSE_1_0.txt.                                      *
 ******************************************************************************/

#define CAF_SUITE fused_streaming

#include "caf/test/dsl.hpp"

#include <memory>
#include <numeric>

#include "caf/actor_system.hpp"
#include "caf/actor_system_config.hpp"
#include "caf/event_based_actor.hpp"
#include "caf/fused_scatterer.hpp"
#include "caf/stateful_actor.hpp"

using std::string;

using namespace caf;

namespace {

TESTEE_SETUP();

using int_scatterer = broadcast_scatterer<int>;

using string_scatterer = broadcast_scatterer<string>;

using ints_atom = atom_constant<atom("ints")>;

using strings_atom = atom_constant<atom("strings")>;

template <class T>
void push(std::deque<T>& xs, downstream<T>& out, size_t num) {
  auto n = std::min(num, xs.size());
  CAF_MESSAGE("push " << n << " messages downstream");
  for (size_t i = 0; i < n; ++i)
    out.push(xs[i]);
  xs.erase(xs.begin(), xs.begin() + static_cast<ptrdiff_t>(n));
}

VARARGS_TESTEE(int_file_reader, size_t buf_size) {
  using buf = std::deque<int>;
  return {
    [=](string& fname) -> output_stream<int> {
      CAF_CHECK_EQUAL(fname, "numbers.txt");
      return self->make_source(
        // initialize state
        [=](buf& xs) {
          xs.resize(buf_size);
          std::iota(xs.begin(), xs.end(), 1);
        },
        // get next element
        [](buf& xs, downstream<int>& out, size_t num) {
          push(xs, out, num);
        },
        // check whether we reached the end
        [=](const buf& xs) {
          return xs.empty();
        });
    }
  };
}

VARARGS_TESTEE(string_file_reader, size_t buf_size) {
  using buf = std::deque<string>;
  return {
    [=](string& fname) -> output_stream<string> {
      CAF_CHECK_EQUAL(fname, "strings.txt");
      return self->make_source(
        // initialize state
        [=](buf& xs) {
          for (size_t i = 0; i < buf_size; ++i)
            xs.emplace_back("some string data");
        },
        // get next element
        [](buf& xs, downstream<string>& out, size_t num) {
          push(xs, out, num);
        },
        // check whether we reached the end
        [=](const buf& xs) {
          return xs.empty();
        });
    }
  };
}

TESTEE_STATE(sum_up) {
  int x = 0;
};

TESTEE(sum_up) {
  return {
    [=](stream<int>& in) {
      return self->make_sink(
        // input stream
        in,
        // initialize state
        [](unit_t&) {
          // nop
        },
        // processing step
        [=](unit_t&, int y) {
          self->state.x += y;
        },
        // cleanup and produce result message
        [=](unit_t&) {
          CAF_MESSAGE(self->name() << " is done");
        }
      );
    },
    [=](join_atom, actor src) {
      CAF_MESSAGE(self->name() << " joins a stream");
      self->send(self * src, join_atom::value, ints_atom::value);
    }
  };
}

TESTEE_STATE(collect) {
  std::vector<string> strings;
};

TESTEE(collect) {
  return {
    [=](stream<string>& in) {
      return self->make_sink(
        // input stream
        in,
        // initialize state
        [](unit_t&) {
          // nop
        },
        // processing step
        [=](unit_t&, string y) {
          self->state.strings.emplace_back(std::move(y));
        },
        // cleanup and produce result message
        [=](unit_t&) {
          CAF_MESSAGE(self->name() << " is done");
        }
      );
    },
    [=](join_atom, actor src) {
      CAF_MESSAGE(self->name() << " joins a stream");
      self->send(self * src, join_atom::value, strings_atom::value);
    }
  };
}

using int_scatterer = broadcast_scatterer<int>;

using string_scatterer = broadcast_scatterer<string>;

using scatterer = fused_scatterer<int_scatterer, string_scatterer>;

class fused_stage : public stream_manager {
public:
  fused_stage(local_actor* self) : stream_manager(self), out_(self) {
    continuous(true);
  }

  bool done() const override {
    return !continuous() && pending_handshakes_ == 0 && inbound_paths_.empty()
           && out_.clean();
  }

  void handle(inbound_path*, downstream_msg::batch& batch) override {
    using std::make_move_iterator;
    using int_vec = std::vector<int>;
    using string_vec = std::vector<string>;
    if (batch.xs.match_elements<int_vec>()) {
      auto& xs = batch.xs.get_mutable_as<int_vec>(0);
      auto& buf = out_.get<int_scatterer>().buf();
      buf.insert(buf.end(), xs.begin(), xs.end());
      return;
    }
    if (batch.xs.match_elements<string_vec>()) {
      auto& xs = batch.xs.get_mutable_as<string_vec>(0);
      auto& buf = out_.get<string_scatterer>().buf();
      buf.insert(buf.end(), xs.begin(), xs.end());
      return;
    }
    CAF_LOG_ERROR("received unexpected batch type (dropped)");
  }

  message make_handshake(stream_slot slot) const override {
    return out_.make_handshake_token(slot);
  }

  bool congested() const noexcept override {
    return out_.capacity() == 0;
  }

  scatterer& out() noexcept override {
    return out_;
  }

private:
  scatterer out_;
  std::map<stream_slot, stream_scatterer*> scatterers_;
};

TESTEE_STATE(stream_multiplexer) {
  intrusive_ptr<fused_stage> stage;
};

TESTEE(stream_multiplexer) {
  self->state.stage = make_counted<fused_stage>(self);
  return {
    [=](join_atom, ints_atom) {
      auto& stg = self->state.stage;
      CAF_MESSAGE("received 'join' request for integers");
      auto result = self->add_unsafe_output_path<int>(stg);
      stg->out().assign<int_scatterer>(result.out());
      return result;
    },
    [=](join_atom, strings_atom) {
      auto& stg = self->state.stage;
      CAF_MESSAGE("received 'join' request for integers");
      auto result = self->add_unsafe_output_path<string>(stg);
      stg->out().assign<string_scatterer>(result.out());
      return result;
    },
    [=](const stream<int>& in) {
      CAF_MESSAGE("received handshake for integers");
      return self->add_unsafe_input_path<void>(in, self->state.stage);
    },
    [=](const stream<string>& in) {
      CAF_MESSAGE("received handshake for strings");
      return self->add_unsafe_input_path<void>(in, self->state.stage);
    }
  };
}

struct fixture : test_coordinator_fixture<> {
  std::chrono::microseconds cycle;

  fixture() : cycle(cfg.streaming_credit_round_interval_us) {
    // Configure the clock to measure each batch item with 1us.
    sched.clock().time_per_unit.emplace(atom("batch"), timespan{1000});
    // Make sure the current time isn't invalid.
    sched.clock().current_time += cycle;
  }
};

} // namespace <anonymous>

// -- unit tests ---------------------------------------------------------------

CAF_TEST_FIXTURE_SCOPE(fused_streaming_tests, fixture)

CAF_TEST(depth_3_pipeline_with_fork) {
  auto src1 = sys.spawn(int_file_reader, 50);
  auto src2 = sys.spawn(string_file_reader, 50);
  auto stg = sys.spawn(stream_multiplexer);
  auto snk1 = sys.spawn(sum_up);
  auto snk2 = sys.spawn(collect);
  auto& st = deref<stream_multiplexer_actor>(stg).state;
  CAF_MESSAGE("connect sinks to the fused stage");
  self->send(snk1, join_atom::value, stg);
  self->send(snk2, join_atom::value, stg);
  sched.run();
  CAF_CHECK_EQUAL(st.stage->out().num_paths(), 2u);
  CAF_CHECK_EQUAL(st.stage->inbound_paths().size(), 0u);
  CAF_MESSAGE("connect sources to the fused stage");
  self->send(stg * src1, "numbers.txt");
  self->send(stg * src2, "strings.txt");
  sched.run();
  CAF_CHECK_EQUAL(st.stage->out().num_paths(), 2u);
  CAF_CHECK_EQUAL(st.stage->inbound_paths().size(), 2u);
  auto predicate = [&] {
    return st.stage->inbound_paths().empty() && st.stage->out().clean();
  };
  sched.run_dispatch_loop(predicate, cycle);
  CAF_CHECK_EQUAL(st.stage->out().num_paths(), 2u);
  CAF_CHECK_EQUAL(st.stage->inbound_paths().size(), 0u);
  CAF_CHECK_EQUAL(deref<sum_up_actor>(snk1).state.x, 1275);
  CAF_CHECK_EQUAL(deref<collect_actor>(snk2).state.strings.size(), 50u);
  self->send_exit(stg, exit_reason::kill);
}

CAF_TEST_FIXTURE_SCOPE_END()
