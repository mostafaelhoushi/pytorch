#include <torch/csrc/autograd/profiler_python.h>

#include <cstdint>
#include <deque>
#include <iostream>
#include <limits>
#include <memory>
#include <queue>
#include <string>
#include <utility>
#include <vector>

#include <Python.h>
#include <frameobject.h>

#include <c10/macros/Macros.h>
#include <c10/util/C++17.h>
#include <c10/util/flat_hash_map.h>
#include <c10/util/irange.h>
#include <torch/csrc/profiler/collection.h>
#include <torch/csrc/profiler/containers.h>
#include <torch/csrc/profiler/util.h>
#include <torch/csrc/utils/pybind.h>
#include <torch/csrc/utils/python_strings.h>

namespace py = pybind11;

namespace torch {
namespace profiler {
namespace impl {
namespace {
enum CallType { PyCall = 0, PyModuleCall, PyCCall };
static constexpr size_t CallTypeSize = 3;

// ============================================================================
// == Miscellaneous structs and utils =========================================
// ============================================================================
struct CodeLocation {
  CodeLocation() = default;
  explicit CodeLocation(const PyFrameObject* frame)
      : code_{frame->f_code}, lasti_{frame->f_lasti} {}

  bool operator==(const CodeLocation& other) const {
    return code_ == other.code_ && lasti_ == other.lasti_;
  }

  PyCodeObject* code_{nullptr};
  int lasti_{0};
};

PyObject* nnModuleCode() {
  static auto module_call_code = []() {
    pybind11::gil_scoped_acquire gil;
    return py::module::import("torch.nn")
        .attr("Module")
        .attr("__call__")
        .attr("__code__")
        .ptr();
  }();
  return module_call_code;
}

} // namespace
} // namespace impl
} // namespace profiler
} // namespace torch

template <>
struct std::hash<torch::profiler::impl::CodeLocation> {
  size_t operator()(const torch::profiler::impl::CodeLocation& x) {
    return c10::get_hash(x.code_, x.lasti_);
  }
};

namespace torch {
namespace profiler {
namespace impl {
namespace {
// ============================================================================
// == CallTypeHelper: Tools for generic programming on specializations. =======
// ============================================================================
template <template <CallType> class ClassT>
class CallTypeHelper final {
 private:
  static_assert(
      CallType::PyCall == 0,
      "CallTypeHelper uses integer math which depends on a zero start.");
  static constexpr size_t End = CallTypeSize;

  template <size_t... I>
  static constexpr std::tuple<ClassT<(CallType)I>...> make_tuple_impl(
      std::index_sequence<I...>);

  template <size_t C, typename T, typename FunctorT, typename... Args>
  static void map(T& t, FunctorT& f, Args... args) {
    f(std::get<C>(t), args...);
    c10::guts::if_constexpr<C + 1 < End>(
        [&](auto _) { map<C + 1>(_(t), f, std::forward<Args>(args)...); });
  }

 public:
  using tuple_type = decltype(make_tuple_impl(std::make_index_sequence<End>{}));

  template <typename FunctorT, typename... Args>
  static void map(tuple_type& t, FunctorT& f, Args... args) {
    map<0>(t, f, std::forward<Args>(args)...);
  }
};

// ============================================================================
// == Event type definitions. =================================================
// ============================================================================
// When we are tracing a Python program, the general procedure is to record
// every time we enter or exit a function and later replay these events during
// post processing. Thus, during the profiling phase we want to do the MINIMAL
// amount of work to capture all of the information that we need; otherwise we
// will distort the profile. (While we don't wish to be terribly inefficient
// during post processing, we are willing to do extra fixup work in post if it
// reduces overhead in the profiling phase.)
//
// When the tracer first enters a frame, it constructs a CallKey for that
// location. The contents of the key vary by context. For a python function
// the key is the (PyCodeObject*, int) pair that defines the bytecode of the
// function. For an `nn.Module` the key is a (non-owning) pointer to `self`.
// For a bound C function it is a (non-owning) pointer to the bound function.
// A CallKey should be small, inexpensive, and POD.
//
// We then collect a CallKey<CallType::PyCall> for the calling frame for better
// source tracking. This pair is a `Callsite`, and serves as a first level key
// during tracing. We lookup the Callsite in a thread local cache which maps
// Callsite to a unique integer `TraceKey`. On a cache hit, we simply store the
// TraceKey and return. On a cache miss, we use a global value cache to store
// whatever fields we need from the two CallKeys, generate a new TraceKey, and
// update the local cache.
//
// During post processing we:
//   1) Determine the type represented by a TraceKey by checking which
//      sub-cache it appears in in the thread local cache.
//   2) Look up the pair of CallKeys from the thread local cache.
//   3) Look up the expanded values of each CallKey from the global value cache.
//
// To add a new event type to the cache:
//   1) Add an entry to the `CallType` enum.
//   2) Add a specialization of Config which defined key_t and cache_t.
//   3) Add a specialization of ValueCache::store and ValueCache::load.

template <CallType>
struct Config;

template <>
struct Config<CallType::PyCall> {
  using key_t = CodeLocation;
  using cache_t = ska::flat_hash_map<key_t, PyFrameState>;
  static constexpr EventType event_type = EventType::PyCall;
};

template <>
struct Config<CallType::PyModuleCall> {
  using key_t = PyModuleSelf;
  struct cache_t {
    c10::optional<CodeLocation> module_forward_;
    ska::flat_hash_map<PyModuleSelf, PyModuleCls> modules_;
    ska::flat_hash_map<PyModuleCls, at::StringView> module_cls_names_;
  };
  static constexpr EventType event_type = EventType::PyCall;
};

template <>
struct Config<CallType::PyCCall> {
  using key_t = torch::profiler::impl::PyCFunction;
  using cache_t = ska::flat_hash_map<key_t, at::StringView>;
  static constexpr EventType event_type = EventType::PyCCall;
};

// ============================================================================
// == Callsite & ValueCache: Storage during profiling =========================
// ============================================================================
template <CallType C>
class Callsite {
 public:
  static constexpr CallType call_type = C;
  using key_t = typename Config<C>::key_t;

  static_assert(
      std::is_trivially_copyable<key_t>::value,
      "Key should be trivial, as it is passed by value.");

  template <typename U>
  Callsite(U value, const PyFrameObject* f_back)
      : value_(value), caller_(f_back) {}

  bool operator==(const Callsite<C>& other) const {
    return value_ == other.value_ && caller_ == other.caller_;
  }

  key_t value_;
  Config<CallType::PyCall>::key_t caller_;
};

class ValueCache {
 public:
  template <CallType C>
  void store(const typename Config<C>::key_t&);

  template <CallType C>
  auto load(const Callsite<C>& callsite, size_t python_tid) const {
    auto caller = load<CallType::PyCall>(callsite.caller_);
    TORCH_INTERNAL_ASSERT(!caller.second.has_value());
    return ExtraFields<Config<C>::event_type>{
        /*end_time_ns=*/std::numeric_limits<time_t>::min(),
        python_tid,
        caller.first,
        load<C>(callsite.value_)};
  }

  void trimPrefixes();

 private:
  template <CallType C>
  typename ExtraFields<Config<C>::event_type>::args_t load(
      const typename Config<C>::key_t&) const;

  template <CallType C>
  using State = typename Config<C>::cache_t;

  CallTypeHelper<State>::tuple_type state_;
};

// ============================================================================
// == Type specific store and load implementations. ===========================
// ============================================================================
using PyCallKey = Config<CallType::PyCall>::key_t;
using PyModuleCallKey = Config<CallType::PyModuleCall>::key_t;
using PyCCallKey = Config<CallType::PyCCall>::key_t;

template <>
void ValueCache::store<CallType::PyCall>(const PyCallKey& key) {
  auto& locations = std::get<CallType::PyCall>(state_);
  if (C10_UNLIKELY(locations.find(key) == locations.end())) {
    TORCH_INTERNAL_ASSERT(key.code_ != nullptr);
    locations[key] = {
        PyCode_Addr2Line(key.code_, key.lasti_),
        at::StringView(THPUtils_unpackString(key.code_->co_filename)),
        at::StringView(THPUtils_unpackString(key.code_->co_name))};
  }
}

template <>
ExtraFields<EventType::PyCall>::args_t ValueCache::load<CallType::PyCall>(
    const PyCallKey& key) const {
  return {std::get<CallType::PyCall>(state_).at(key), c10::nullopt};
}

template <>
void ValueCache::store<CallType::PyModuleCall>(const PyModuleCallKey& key) {
  auto& cache = std::get<CallType::PyModuleCall>(state_);
  if (C10_UNLIKELY(cache.modules_.find(key) == cache.modules_.end())) {
    if (C10_UNLIKELY(!cache.module_forward_.has_value())) {
      auto frame = PyEval_GetFrame();
      TORCH_INTERNAL_ASSERT((PyObject*)(frame->f_code) == nnModuleCode());
      cache.module_forward_ = PyCallKey(frame);
      store<CallType::PyCall>(*cache.module_forward_);
    }
    auto cls_handle = py::handle((PyObject*)key).attr("__class__");
    auto cls = PyModuleCls(cls_handle.ptr());
    cache.modules_[key] = cls;

    if (cache.module_cls_names_.find(cls) == cache.module_cls_names_.end()) {
      cache.module_cls_names_[cls] =
          at::StringView(py::str(cls_handle.attr("__name__")));
    }
  }
}

template <>
ExtraFields<EventType::PyCall>::args_t ValueCache::load<CallType::PyModuleCall>(
    const PyModuleCallKey& key) const {
  auto& cache = std::get<CallType::PyModuleCall>(state_);
  TORCH_INTERNAL_ASSERT(cache.module_forward_.has_value());
  auto cls = cache.modules_.at(key);
  auto fwd = std::get<CallType::PyCall>(state_).at(*cache.module_forward_);
  return {fwd, NNModuleInfo{key, cls, cache.module_cls_names_.at(cls)}};
}

template <>
void ValueCache::store<CallType::PyCCall>(const PyCCallKey& key) {
  auto& names = std::get<CallType::PyCCall>(state_);
  if (C10_UNLIKELY(names.find(key) == names.end())) {
    names[key] = at::StringView(py::repr((PyObject*)key));
  }
}

template <>
ExtraFields<EventType::PyCCall>::args_t ValueCache::load<CallType::PyCCall>(
    const PyCCallKey& key) const {
  return std::get<CallType::PyCCall>(state_).at(key);
}

// TODO: Use re2.
void ValueCache::trimPrefixes() {
  static const auto prefixes = []() {
    pybind11::gil_scoped_acquire gil;
    return py::module::import("torch.profiler.python_tracer")
        .attr("_prefix_regex")()
        .cast<std::vector<std::string>>();
  }();

  for (auto& it : std::get<CallType::PyCall>(state_)) {
    std::string filename = it.second.filename_.str();
    for (const auto& p : prefixes) {
      if (filename.compare(0, p.size(), p) == 0) {
        filename.erase(0, p.size());
        it.second.filename_ = at::StringView(filename);
        break;
      }
    }
  }
}

// ============================================================================
// == TraceKey cache ==========================================================
// ============================================================================
using python_tracer::TraceKey;

TraceKey nextKey() {
  static std::atomic<uint64_t> key{0};
  return TraceKey{++key};
}

template <CallType C>
struct TraceKeyCacheState {
  struct Hash {
    size_t operator()(const Callsite<C>& key) {
      return c10::get_hash(key.value_, key.caller_);
    }
  };

  TraceKey intern(Callsite<C> callsite, ValueCache& value_cache) {
    auto it = state_.find(callsite);
    if (C10_UNLIKELY(it == state_.end())) {
      value_cache.store<C>(callsite.value_);
      value_cache.store<CallType::PyCall>(callsite.caller_);
      it = state_.insert({callsite, nextKey()}).first;
    }
    return it->second;
  }

  auto lookup(Callsite<C>& callsite, ValueCache& value_cache) const {
    return std::make_pair(
        value_cache.load<C>(callsite.value_),
        value_cache.load<CallType::PyCall>(callsite.caller_));
  }

  ska::flat_hash_map<Callsite<C>, TraceKey, Hash> state_;
};

// ============================================================================
// == Core CPython data types =================================================
// ============================================================================
// PyObject that allows different threads to record events without colliding.
// It is passed as the second argument when enabling tracing via
// `PyEval_SetProfile`.
struct ThreadLocalResults;
struct TraceContext {
  PyObject_HEAD ThreadLocalResults* thread_local_results_;
};

// CPython boilerplate to define `TraceContext` as a proper python object.
static PyTypeObject TraceContextType = {
    PyVarObject_HEAD_INIT(nullptr, 0) "TraceContext", /* tp_name */
    sizeof(TraceContext), /* tp_basicsize */
    0, /* tp_itemsize */
    nullptr, /* tp_dealloc */
    0,
    /* tp_vectorcall_offset */ // NOLINT: modernize-use-nullptr
    nullptr, /* tp_getattr */
    nullptr, /* tp_setattr */
    nullptr, /* tp_reserved */
    nullptr, /* tp_repr */
    nullptr, /* tp_as_number */
    nullptr, /* tp_as_sequence */
    nullptr, /* tp_as_mapping */
    nullptr, /* tp_hash  */
    nullptr, /* tp_call */
    nullptr, /* tp_str */
    nullptr, /* tp_getattro */
    nullptr, /* tp_setattro */
    nullptr, /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT, /* tp_flags */
    "Python tracer TLS", /* tp_doc */
    nullptr, /* tp_traverse */
    nullptr, /* tp_clear */
    nullptr, /* tp_richcompare */
    0, /* tp_weaklistoffset */
    nullptr, /* tp_iter */
    nullptr, /* tp_iternext */
    nullptr, /* tp_methods */
    nullptr, /* tp_members */
    nullptr, /* tp_getset */
    nullptr, /* tp_base */
    nullptr, /* tp_dict */
    nullptr, /* tp_descr_get */
    nullptr, /* tp_descr_set */
    0, /* tp_dictoffset */
    nullptr, /* tp_init */
    nullptr, /* tp_alloc */
    PyType_GenericNew, /* tp_new */
    nullptr /* tp_free */
};

// ============================================================================
// == Thread local cache ======================================================
// ============================================================================
struct ThreadLocalResults {
  ThreadLocalResults(PyThreadState* thread_state, ValueCache* value_cache)
      : thread_state_{thread_state},
        ctx_{(TraceContext*)TraceContextType.tp_alloc(&TraceContextType, 0)},
        value_cache_{value_cache} {
    ctx_->thread_local_results_ = this;
  }

  ThreadLocalResults() = delete;
  ThreadLocalResults(const ThreadLocalResults&) = delete;
  ThreadLocalResults(ThreadLocalResults&&) = delete;
  ThreadLocalResults& operator=(const ThreadLocalResults&) = delete;
  ThreadLocalResults& operator=(const ThreadLocalResults&&) = delete;

  ~ThreadLocalResults() {
    Py_DECREF((PyObject*)ctx_);
  }

  template <CallType C, EventType E, typename... Args>
  TraceKey intern(Args... args) {
    static_assert(
        Config<C>::event_type == E,
        "ThreadLocalResults.intern called from the wrong typed context.");
    return std::get<C>(trace_keys_)
        .intern(Callsite<C>(std::forward<Args>(args)...), *value_cache_);
  }

  static constexpr size_t BLOCK_SIZE = 1024;

  PyThreadState* thread_state_;
  TraceContext* ctx_;
  ValueCache* value_cache_;
  CallTypeHelper<TraceKeyCacheState>::tuple_type trace_keys_;
  AppendOnlyList<approx_time_t, BLOCK_SIZE> exit_times_;
  AppendOnlyList<approx_time_t, BLOCK_SIZE> c_exit_times_;
};

// ============================================================================
// == Tracing implementation ==================================================
// ============================================================================
class PythonTracer final : public python_tracer::PythonTracerBase {
 public:
  static int pyProfileFn(
      PyObject* obj,
      PyFrameObject* frame,
      int what,
      PyObject* arg);

  static PythonTracer& singleton();
  void start(torch::profiler::impl::RecordQueue* queue) override;
  void stop() override;
  std::vector<std::shared_ptr<Result>> getEvents(
      std::function<time_t(approx_time_t)> time_converter,
      std::vector<python_tracer::CompressedEvent>& enters) override;
  void clear() override;

 private:
  PythonTracer();

  void recordPyCall(ThreadLocalResults& tls, PyFrameObject* frame);
  void recordCCall(
      ThreadLocalResults& tls,
      PyFrameObject* frame,
      PyObject* arg);

  torch::profiler::impl::RecordQueue* queue_;
  PyObject* module_call_code_;

  std::deque<ThreadLocalResults> thread_local_results_;
  ValueCache value_cache_;
};

PythonTracer& PythonTracer::singleton() {
  static PythonTracer singleton_;
  return singleton_;
}

PythonTracer::PythonTracer()
    : queue_(nullptr), module_call_code_(nnModuleCode()) {}

void PythonTracer::start(torch::profiler::impl::RecordQueue* queue) {
  TORCH_CHECK(queue_ == nullptr, "PythonTracer is already active")
  TORCH_CHECK(
      !thread_local_results_.size(),
      "PythonTracer should not have active contexts");
  queue_ = queue;

  pybind11::gil_scoped_acquire gil;

  // Loop over all threads within the current interpreter. We will need to
  // register a trace function with each thread. We set the current thread to
  // position zero to ensure that it is traced, and so we can restore the
  // thread state after registration. The profiler cannot post process multiple
  // python threads yet, so this section is temporarily disabled.
  std::vector<PyThreadState*> thread_states{PyThreadState_Get()};
  /*
  if (all_threads) {
    auto thread_state = thread_states[0];
    while (thread_state != nullptr) {
      if (thread_state != thread_states[0]) {
        thread_states.push_back(thread_state);
      }
      thread_state = PyThreadState_Next(thread_state);
    }
  }
  */

  // Register the tracer in each thread.
  for (const auto i : c10::irange(thread_states.size())) {
    PyThreadState* thread_state = thread_states[i];
    PyThreadState_Swap(thread_state);

    thread_local_results_.emplace_back(thread_state, &value_cache_);
    auto* ctx = thread_local_results_.back().ctx_;

    // When we begin profiling there are already frames on the Python
    // interpreter stack. To ensure a complete trace, we must push calls
    // to all the prior frames onto our event stack. (We stop at depth=128)
    std::vector<PyFrameObject*> current_stack;
    auto frame = PyEval_GetFrame();
    size_t depth = 0; // Make sure we can't infinite loop.
    while (frame != nullptr && depth <= 128) {
      current_stack.push_back(frame);
      frame = frame->f_back;
      depth++;
    }
    for (auto it = current_stack.rbegin(); it != current_stack.rend(); it++) {
      recordPyCall(thread_local_results_.back(), *it);
    }

    // Note:
    //   This profile will not compose with other CPython profilers, and
    //   cannot be round tripped via `sys.settrace(sys.gettrace())`
    PyEval_SetProfile(PythonTracer::pyProfileFn, (PyObject*)ctx);
  }

  // Restore the thread state to its initial value.
  PyThreadState_Swap(thread_states[0]);
};

void PythonTracer::stop() {
  TORCH_INTERNAL_ASSERT(queue_ != nullptr, "PythonTracer is not running.")
  queue_ = nullptr;

  pybind11::gil_scoped_acquire gil;

  PyThreadState* initial_thread_state = PyThreadState_Get();
  for (const auto& i : thread_local_results_) {
    PyThreadState_Swap(i.thread_state_);
    PyEval_SetProfile(nullptr, nullptr);
  }
  PyThreadState_Swap(initial_thread_state);
}

void PythonTracer::clear() {
  TORCH_CHECK(
      queue_ == nullptr, "Cannot clear state while PythonTracer is active.");
  thread_local_results_.clear();
  value_cache_ = ValueCache();
}

void PythonTracer::recordPyCall(ThreadLocalResults& tls, PyFrameObject* frame) {
  static constexpr auto E = EventType::PyCall;
  auto get_key = [&]() -> TraceKey {
    if ((PyObject*)(frame->f_code) == module_call_code_) {
      // By default, CPython stores locals in a "fast" format, with an array
      // of names and an array of values. Consequently, frame->f_locals is
      // NULL since the interpreter has no need to populate it.
      //
      // If these arrays were part of the public API then we could very
      // quickly access `self`. Unfortunately they are not, and moreover are
      // not stable across versions. As a result, we are forced to call
      // `PyFrame_FastToLocals` which forces the interpreter to materialize
      // the full dict of locals.
      PyFrame_FastToLocals(frame);
      auto self = PyDict_GetItemString(frame->f_locals, "self");
      PyFrame_LocalsToFast(frame, 0);
      TORCH_INTERNAL_ASSERT(frame->f_back != nullptr);
      return tls.intern<CallType::PyModuleCall, E>(self, frame->f_back);

    } else {
      auto f_back = frame->f_back != nullptr ? frame->f_back : frame;
      return tls.intern<CallType::PyCall, E>(frame, f_back);
    }
  };
  queue_->getSubqueue()->emplace_py_call(get_key(), getApproximateTime());
}

void PythonTracer::recordCCall(
    ThreadLocalResults& tls,
    PyFrameObject* frame,
    PyObject* arg) {
  // NB: For C calls a new frame is not created, so we use `frame` rather than
  //     `frame->f_back`.
  auto key = tls.intern<CallType::PyCCall, EventType::PyCCall>(arg, frame);
  queue_->getSubqueue()->emplace_py_call(key, getApproximateTime());
}

// ============================================================================
// == Post processing =========================================================
// ============================================================================
struct Exit {
  bool operator>(const Exit& other) const {
    return t_ > other.t_;
  }

  time_t t_;
  size_t python_tid_;
};

class PostProcess {
 public:
  PostProcess(
      std::function<time_t(approx_time_t)> time_converter,
      std::deque<ThreadLocalResults>& tls,
      const ValueCache& value_cache)
      : time_converter_{time_converter} {
    for (size_t python_tid : c10::irange(tls.size())) {
      CallTypeHelper<TraceKeyCacheState>::map(
          tls[python_tid].trace_keys_, *this, value_cache, python_tid);

      addExits<EventType::PyCall>(tls[python_tid].exit_times_, python_tid);
      addExits<EventType::PyCCall>(tls[python_tid].c_exit_times_, python_tid);
    }
  }

  template <CallType C>
  void operator()(
      const TraceKeyCacheState<C>& trace_cache,
      const ValueCache& value_cache,
      size_t python_tid) {
    for (const auto& it : trace_cache.state_) {
      const auto inserted = get_state<Config<C>::event_type>().fields_.insert(
          {it.second, value_cache.load(it.first, python_tid)});
      TORCH_INTERNAL_ASSERT(inserted.second, "Duplicate key: ", it.second);
    }
  }

  template <EventType E, size_t N>
  void addExits(AppendOnlyList<approx_time_t, N>& exits, size_t python_tid) {
    for (const auto i : exits) {
      get_state<E>().exits_.push({time_converter_(i), python_tid});
    }
  }

  std::vector<std::shared_ptr<Result>> run(
      std::vector<python_tracer::CompressedEvent>& enters) {
    std::stable_sort(
        enters.begin(), enters.end(), [](const auto a, const auto b) {
          return a.enter_t_ < b.enter_t_;
        });
    std::vector<std::shared_ptr<Result>> out;
    populate<EventType::PyCall>(enters, out);
    populate<EventType::PyCCall>(enters, out);
    return out;
  }

 private:
  template <EventType E>
  void populate(
      std::vector<python_tracer::CompressedEvent>& enters,
      std::vector<std::shared_ptr<Result>>& out) {
    using stack_t = std::vector<std::shared_ptr<Result>>;
    ska::flat_hash_map<size_t, stack_t> stacks;
    auto& state = get_state<E>();
    for (const auto& enter : enters) {
      auto fields_it = state.fields_.find(enter.key_);
      if (fields_it != state.fields_.end()) {
        while (!state.exits_.empty() &&
               state.exits_.top().t_ < enter.enter_t_) {
          auto& stack = stacks[state.exits_.top().python_tid_];
          TORCH_INTERNAL_ASSERT(stack.size(), "Python replay stack is empty.");
          c10::get<ExtraFields<E>>(stack.back()->extra_fields_).end_time_ns_ =
              state.exits_.top().t_;
          state.exits_.pop();
          stack.pop_back();
        }
        out.push_back(Result::create(
            enter.enter_t_,
            enter.system_tid_,
            enter.kineto_info_,
            fields_it->second));

        stacks[fields_it->second.python_tid_].push_back(out.back());
      }
    }
  }

  template <EventType E>
  struct State {
    ska::flat_hash_map<TraceKey, ExtraFields<E>> fields_;
    std::priority_queue<Exit, std::vector<Exit>, std::greater<Exit>> exits_;
  };

  template <EventType E>
  auto& get_state() {
    return std::get < E == EventType::PyCall ? 0 : 1 > (state_);
  }

  std::function<time_t(approx_time_t)> time_converter_;
  std::tuple<State<EventType::PyCall>, State<EventType::PyCCall>> state_;
};

struct PythonIDVisitor {
  void operator()(ExtraFields<EventType::PyCall>& py_call) {
    py_call.id_ = ++current_python_id_;
    if (py_call.module_.has_value()) {
      auto& m = py_call.module_;
      auto& module_ids = module_ids_[m->cls_];
      m->id_ = module_ids.insert({m->self_, module_ids.size()}).first->second;
    }
  }

  void operator()(ExtraFields<EventType::PyCCall>& py_call) {
    py_call.id_ = ++current_python_id_;
  }

  template <typename T>
  void operator()(T&) {}

  size_t current_python_id_{0};
  ska::flat_hash_map<PyModuleCls, ska::flat_hash_map<PyModuleSelf, size_t>>
      module_ids_;
};

std::vector<std::shared_ptr<Result>> PythonTracer::getEvents(
    std::function<time_t(approx_time_t)> time_converter,
    std::vector<python_tracer::CompressedEvent>& enters) {
  value_cache_.trimPrefixes();
  PostProcess post_process(time_converter, thread_local_results_, value_cache_);
  auto out = post_process.run(enters);

  std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
    return a->start_time_ns_ < b->start_time_ns_;
  });

  PythonIDVisitor id_visitor;
  for (auto& i : out) {
    c10::visit(id_visitor, i->extra_fields_);
  }

  return out;
}

// ============================================================================
// == API =====================================================================
// ============================================================================
int PythonTracer::pyProfileFn(
    PyObject* obj,
    PyFrameObject* frame,
    int what,
    PyObject* arg) {
  auto& local_results =
      *reinterpret_cast<TraceContext*>(obj)->thread_local_results_;
  switch (what) {
    case PyTrace_CALL:
      PythonTracer::singleton().recordPyCall(local_results, frame);
      break;

    case PyTrace_C_CALL:
      PythonTracer::singleton().recordCCall(local_results, frame, arg);
      break;

    case PyTrace_EXCEPTION:
    case PyTrace_RETURN:
      local_results.exit_times_.emplace_back(getApproximateTime());
      break;

    case PyTrace_C_EXCEPTION:
    case PyTrace_C_RETURN:
      local_results.c_exit_times_.emplace_back(getApproximateTime());
      break;
  }
  return 0;
}

python_tracer::PythonTracerBase& getTracer() {
  return PythonTracer::singleton();
}
} // namespace
} // namespace impl
} // namespace profiler
} // namespace torch

namespace torch {
namespace autograd {
namespace profiler {
namespace python_tracer {

void init() {
  pybind11::gil_scoped_acquire gil;
  TORCH_CHECK(PyType_Ready(&torch::profiler::impl::TraceContextType) == 0);
  torch::profiler::impl::python_tracer::registerTracer(
      &torch::profiler::impl::getTracer);
}
} // namespace python_tracer
} // namespace profiler
} // namespace autograd
} // namespace torch
