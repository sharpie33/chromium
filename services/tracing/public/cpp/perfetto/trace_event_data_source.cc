// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/tracing/public/cpp/perfetto/trace_event_data_source.h"

#include <atomic>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/command_line.h"
#include "base/debug/leak_annotations.h"
#include "base/json/json_writer.h"
#include "base/memory/ref_counted_memory.h"
#include "base/metrics/histogram_samples.h"
#include "base/metrics/metrics_hashes.h"
#include "base/metrics/statistics_recorder.h"
#include "base/metrics/user_metrics.h"
#include "base/no_destructor.h"
#include "base/pickle.h"
#include "base/sequence_checker.h"
#include "base/strings/pattern.h"
#include "base/strings/string_number_conversions.h"
#include "base/synchronization/lock.h"
#include "base/task/common/scoped_defer_task_posting.h"
#include "base/threading/sequenced_task_runner_handle.h"
#include "base/time/time.h"
#include "base/trace_event/trace_config.h"
#include "base/trace_event/trace_event.h"
#include "base/trace_event/trace_log.h"
#include "build/build_config.h"
#include "components/tracing/common/tracing_switches.h"
#include "services/tracing/public/cpp/perfetto/macros.h"
#include "services/tracing/public/cpp/perfetto/perfetto_producer.h"
#include "services/tracing/public/cpp/perfetto/perfetto_traced_process.h"
#include "services/tracing/public/cpp/perfetto/trace_time.h"
#include "services/tracing/public/cpp/perfetto/traced_value_proto_writer.h"
#include "services/tracing/public/cpp/perfetto/track_event_thread_local_event_sink.h"
#include "services/tracing/public/cpp/trace_event_args_whitelist.h"
#include "services/tracing/public/cpp/trace_startup.h"
#include "services/tracing/public/mojom/constants.mojom.h"
#include "third_party/perfetto/include/perfetto/ext/tracing/core/shared_memory_arbiter.h"
#include "third_party/perfetto/include/perfetto/ext/tracing/core/startup_trace_writer.h"
#include "third_party/perfetto/include/perfetto/ext/tracing/core/startup_trace_writer_registry.h"
#include "third_party/perfetto/include/perfetto/ext/tracing/core/trace_writer.h"
#include "third_party/perfetto/include/perfetto/tracing/track.h"
#include "third_party/perfetto/protos/perfetto/trace/chrome/chrome_metadata.pbzero.h"
#include "third_party/perfetto/protos/perfetto/trace/chrome/chrome_trace_event.pbzero.h"
#include "third_party/perfetto/protos/perfetto/trace/trace_packet.pbzero.h"
#include "third_party/perfetto/protos/perfetto/trace/track_event/chrome_histogram_sample.pbzero.h"
#include "third_party/perfetto/protos/perfetto/trace/track_event/chrome_process_descriptor.pbzero.h"
#include "third_party/perfetto/protos/perfetto/trace/track_event/chrome_user_event.pbzero.h"
#include "third_party/perfetto/protos/perfetto/trace/track_event/process_descriptor.pbzero.h"
#include "third_party/perfetto/protos/perfetto/trace/track_event/track_descriptor.pbzero.h"

#if defined(OS_ANDROID)
#include "base/android/build_info.h"
#endif

using TraceLog = base::trace_event::TraceLog;
using TraceEvent = base::trace_event::TraceEvent;
using TraceConfig = base::trace_event::TraceConfig;
using TracePacketHandle = perfetto::TraceWriter::TracePacketHandle;
using TraceRecordMode = base::trace_event::TraceRecordMode;
using perfetto::protos::pbzero::ChromeMetadataPacket;
using perfetto::protos::pbzero::ChromeProcessDescriptor;
using perfetto::protos::pbzero::ProcessDescriptor;
using perfetto::protos::pbzero::TrackDescriptor;

namespace tracing {
namespace {

TraceEventMetadataSource* g_trace_event_metadata_source_for_testing = nullptr;

ChromeProcessDescriptor::ProcessType GetProcessType(const std::string& name) {
  if (name == "Browser") {
    return ChromeProcessDescriptor::PROCESS_BROWSER;
  } else if (name == "Renderer") {
    return ChromeProcessDescriptor::PROCESS_RENDERER;
  } else if (name == "GPU Process") {
    return ChromeProcessDescriptor::PROCESS_GPU;
  } else if (base::MatchPattern(name, "Service:*")) {
    return ChromeProcessDescriptor::PROCESS_UTILITY;
  } else if (name == "HeadlessBrowser") {
    return ChromeProcessDescriptor::PROCESS_BROWSER;
  } else if (name == "PPAPI Process") {
    return ChromeProcessDescriptor::PROCESS_PPAPI_PLUGIN;
  } else if (name == "PPAPI Broker Process") {
    return ChromeProcessDescriptor::PROCESS_PPAPI_BROKER;
  }
  return ChromeProcessDescriptor::PROCESS_UNSPECIFIED;
}

void WriteMetadataProto(ChromeMetadataPacket* metadata_proto,
                        bool privacy_filtering_enabled) {
#if defined(OS_ANDROID) && defined(OFFICIAL_BUILD)
  // Version code is only set for official builds on Android.
  const char* version_code_str =
      base::android::BuildInfo::GetInstance()->package_version_code();
  if (version_code_str) {
    int version_code = 0;
    bool res = base::StringToInt(version_code_str, &version_code);
    DCHECK(res);
    metadata_proto->set_chrome_version_code(version_code);
  }
#endif  // defined(OS_ANDROID) && defined(OFFICIAL_BUILD)
}

static_assert(
    sizeof(TraceEventDataSource::SessionFlags) <= sizeof(uint64_t),
    "SessionFlags should remain small to ensure lock-free atomic operations");

// Helper class used to ensure no tasks are posted while
// TraceEventDataSource::lock_ is held.
class AutoLockWithDeferredTaskPosting {
 public:
  explicit AutoLockWithDeferredTaskPosting(base::Lock& lock)
      : autolock_(lock) {}

 private:
  // The ordering is important: |defer_task_posting_| must be destroyed
  // after |autolock_| to ensure the lock is not held when any deferred
  // tasks are posted..
  base::ScopedDeferTaskPosting defer_task_posting_;
  base::AutoLock autolock_;
};

}  // namespace

using perfetto::protos::pbzero::ChromeEventBundle;
using ChromeEventBundleHandle = protozero::MessageHandle<ChromeEventBundle>;

// static
TraceEventMetadataSource* TraceEventMetadataSource::GetInstance() {
  static base::NoDestructor<TraceEventMetadataSource> instance;
  return instance.get();
}

TraceEventMetadataSource::TraceEventMetadataSource()
    : DataSourceBase(mojom::kMetaDataSourceName),
      origin_task_runner_(base::SequencedTaskRunnerHandle::Get()) {
  g_trace_event_metadata_source_for_testing = this;
  PerfettoTracedProcess::Get()->AddDataSource(this);
  AddGeneratorFunction(base::BindRepeating(&WriteMetadataProto));
  AddGeneratorFunction(base::BindRepeating(
      &TraceEventMetadataSource::GenerateTraceConfigMetadataDict,
      base::Unretained(this)));
}

TraceEventMetadataSource::~TraceEventMetadataSource() = default;

void TraceEventMetadataSource::AddGeneratorFunction(
    JsonMetadataGeneratorFunction generator) {
  DCHECK(origin_task_runner_->RunsTasksInCurrentSequence());
  json_generator_functions_.push_back(generator);
  // An EventBundle is created when nullptr is passed.
  GenerateJsonMetadataFromGenerator(generator, nullptr);
}

void TraceEventMetadataSource::AddGeneratorFunction(
    MetadataGeneratorFunction generator) {
  DCHECK(origin_task_runner_->RunsTasksInCurrentSequence());
  generator_functions_.push_back(generator);
  GenerateMetadataFromGenerator(generator);
}

std::unique_ptr<base::DictionaryValue>
TraceEventMetadataSource::GenerateTraceConfigMetadataDict() {
  if (chrome_config_.empty()) {
    return nullptr;
  }

  auto metadata_dict = std::make_unique<base::DictionaryValue>();
  // If argument filtering is enabled, we need to check if the trace config is
  // whitelisted before emitting it.
  // TODO(eseckler): Figure out a way to solve this without calling directly
  // into IsMetadataWhitelisted().
  if (!parsed_chrome_config_->IsArgumentFilterEnabled() ||
      IsMetadataWhitelisted("trace-config")) {
    metadata_dict->SetString("trace-config", chrome_config_);
  } else {
    metadata_dict->SetString("trace-config", "__stripped__");
  }

  chrome_config_ = std::string();
  return metadata_dict;
}

void TraceEventMetadataSource::GenerateMetadataFromGenerator(
    const TraceEventMetadataSource::MetadataGeneratorFunction& generator) {
  DCHECK(origin_task_runner_->RunsTasksInCurrentSequence());
  perfetto::TraceWriter::TracePacketHandle trace_packet;
  {
    AutoLockWithDeferredTaskPosting lock(lock_);
    if (!emit_metadata_at_start_ || !trace_writer_) {
      return;
    }
    trace_packet = trace_writer_->NewTracePacket();
  }
  trace_packet->set_timestamp(
      TRACE_TIME_TICKS_NOW().since_origin().InNanoseconds());
  trace_packet->set_timestamp_clock_id(kTraceClockId);
  auto* chrome_metadata = trace_packet->set_chrome_metadata();
  generator.Run(chrome_metadata, privacy_filtering_enabled_);
}

void TraceEventMetadataSource::GenerateJsonMetadataFromGenerator(
    const TraceEventMetadataSource::JsonMetadataGeneratorFunction& generator,
    ChromeEventBundle* event_bundle) {
  DCHECK(origin_task_runner_->RunsTasksInCurrentSequence());
  perfetto::TraceWriter::TracePacketHandle trace_packet;
  if (!event_bundle) {
    {
      AutoLockWithDeferredTaskPosting lock(lock_);
      if (!emit_metadata_at_start_ || !trace_writer_) {
        return;
      }
      trace_packet = trace_writer_->NewTracePacket();
    }
    trace_packet->set_timestamp(
        TRACE_TIME_TICKS_NOW().since_origin().InNanoseconds());
    trace_packet->set_timestamp_clock_id(kTraceClockId);
    event_bundle = trace_packet->set_chrome_events();
  }

  std::unique_ptr<base::DictionaryValue> metadata_dict = generator.Run();
  if (!metadata_dict) {
    return;
  }

  for (const auto& it : metadata_dict->DictItems()) {
    auto* new_metadata = event_bundle->add_metadata();
    new_metadata->set_name(it.first.c_str());

    if (it.second.is_int()) {
      new_metadata->set_int_value(it.second.GetInt());
    } else if (it.second.is_bool()) {
      new_metadata->set_bool_value(it.second.GetBool());
    } else if (it.second.is_string()) {
      new_metadata->set_string_value(it.second.GetString().c_str());
    } else {
      std::string json_value;
      base::JSONWriter::Write(it.second, &json_value);
      new_metadata->set_json_value(json_value.c_str());
    }
  }
}

std::unique_ptr<base::DictionaryValue>
TraceEventMetadataSource::GenerateLegacyMetadataDict() {
  DCHECK(!privacy_filtering_enabled_);

  auto merged_metadata = std::make_unique<base::DictionaryValue>();
  for (auto& generator : json_generator_functions_) {
    std::unique_ptr<base::DictionaryValue> metadata_dict = generator.Run();
    if (!metadata_dict) {
      continue;
    }
    merged_metadata->MergeDictionary(metadata_dict.get());
  }

  base::trace_event::MetadataFilterPredicate metadata_filter =
      base::trace_event::TraceLog::GetInstance()->GetMetadataFilterPredicate();

  // This out-of-band generation of the global metadata is only used by the
  // crash service uploader path, which always requires privacy filtering.
  CHECK(metadata_filter);
  for (base::DictionaryValue::Iterator it(*merged_metadata); !it.IsAtEnd();
       it.Advance()) {
    if (!metadata_filter.Run(it.key())) {
      merged_metadata->SetString(it.key(), "__stripped__");
    }
  }

  return merged_metadata;
}

void TraceEventMetadataSource::GenerateMetadata(
    std::unique_ptr<
        std::vector<TraceEventMetadataSource::JsonMetadataGeneratorFunction>>
        json_generators,
    std::unique_ptr<
        std::vector<TraceEventMetadataSource::MetadataGeneratorFunction>>
        proto_generators) {
  DCHECK(origin_task_runner_->RunsTasksInCurrentSequence());
  TracePacketHandle trace_packet;
  bool privacy_filtering_enabled;
  {
    AutoLockWithDeferredTaskPosting lock(lock_);
    trace_packet = trace_writer_->NewTracePacket();
    privacy_filtering_enabled = privacy_filtering_enabled_;
  }

  trace_packet->set_timestamp(
      TRACE_TIME_TICKS_NOW().since_origin().InNanoseconds());
  trace_packet->set_timestamp_clock_id(kTraceClockId);
  auto* chrome_metadata = trace_packet->set_chrome_metadata();
  for (auto& generator : *proto_generators) {
    generator.Run(chrome_metadata, privacy_filtering_enabled_);
  }

  if (privacy_filtering_enabled) {
    return;
  }

  ChromeEventBundle* event_bundle = trace_packet->set_chrome_events();

  for (auto& generator : *json_generators) {
    GenerateJsonMetadataFromGenerator(generator, event_bundle);
  }
}

void TraceEventMetadataSource::StartTracing(
    PerfettoProducer* producer,
    const perfetto::DataSourceConfig& data_source_config) {
  auto json_generators =
      std::make_unique<std::vector<JsonMetadataGeneratorFunction>>();
  auto proto_generators =
      std::make_unique<std::vector<MetadataGeneratorFunction>>();
  {
    AutoLockWithDeferredTaskPosting lock(lock_);
    privacy_filtering_enabled_ =
        data_source_config.chrome_config().privacy_filtering_enabled();
    chrome_config_ = data_source_config.chrome_config().trace_config();
    parsed_chrome_config_ = std::make_unique<TraceConfig>(chrome_config_);
    trace_writer_ =
        producer->CreateTraceWriter(data_source_config.target_buffer());
    switch (parsed_chrome_config_->GetTraceRecordMode()) {
      case TraceRecordMode::RECORD_UNTIL_FULL:
      case TraceRecordMode::RECORD_AS_MUCH_AS_POSSIBLE: {
        emit_metadata_at_start_ = true;
        *json_generators = json_generator_functions_;
        *proto_generators = generator_functions_;
        break;
      }
      case TraceRecordMode::RECORD_CONTINUOUSLY:
      case TraceRecordMode::ECHO_TO_CONSOLE:
        emit_metadata_at_start_ = false;
        return;
    }
  }
  // |emit_metadata_at_start_| is true if we are in discard packets mode, write
  // metadata at the beginning of the trace to make it less likely to be
  // dropped.
  origin_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&TraceEventMetadataSource::GenerateMetadata,
                     base::Unretained(this), std::move(json_generators),
                     std::move(proto_generators)));
}

void TraceEventMetadataSource::StopTracing(
    base::OnceClosure stop_complete_callback) {
  base::OnceClosure maybe_generate_task = base::DoNothing();
  {
    AutoLockWithDeferredTaskPosting lock(lock_);
    if (!emit_metadata_at_start_ && trace_writer_) {
      // Write metadata at the end of tracing if not emitted at start (in ring
      // buffer mode), to make it less likely that it is overwritten by other
      // trace data in perfetto's ring buffer.
      auto json_generators =
          std::make_unique<std::vector<JsonMetadataGeneratorFunction>>();
      *json_generators = json_generator_functions_;
      auto proto_generators =
          std::make_unique<std::vector<MetadataGeneratorFunction>>();
      *proto_generators = generator_functions_;
      maybe_generate_task = base::BindOnce(
          &TraceEventMetadataSource::GenerateMetadata, base::Unretained(this),
          std::move(json_generators), std::move(proto_generators));
    }
  }
  // Even when not generating metadata, make sure the metadata generate task
  // posted at the start is finished, by posting task on origin task runner.
  origin_task_runner_->PostTaskAndReply(
      FROM_HERE, std::move(maybe_generate_task),
      base::BindOnce(
          [](TraceEventMetadataSource* ds,
             base::OnceClosure stop_complete_callback) {
            {
              AutoLockWithDeferredTaskPosting lock(ds->lock_);
              ds->producer_ = nullptr;
              ds->trace_writer_.reset();
              ds->chrome_config_ = std::string();
              ds->parsed_chrome_config_.reset();
              ds->emit_metadata_at_start_ = false;
            }
            std::move(stop_complete_callback).Run();
          },
          base::Unretained(this), std::move(stop_complete_callback)));
}

void TraceEventMetadataSource::Flush(
    base::RepeatingClosure flush_complete_callback) {
  origin_task_runner_->PostTaskAndReply(FROM_HERE, base::DoNothing(),
                                        std::move(flush_complete_callback));
}

void TraceEventMetadataSource::ResetForTesting() {
  if (!g_trace_event_metadata_source_for_testing)
    return;
  g_trace_event_metadata_source_for_testing->~TraceEventMetadataSource();
  new (g_trace_event_metadata_source_for_testing) TraceEventMetadataSource;
}

namespace {

base::ThreadLocalStorage::Slot* ThreadLocalEventSinkSlot() {
  static base::NoDestructor<base::ThreadLocalStorage::Slot>
      thread_local_event_sink_tls([](void* event_sink) {
        AutoThreadLocalBoolean thread_is_in_trace_event(
            TraceEventDataSource::GetThreadIsInTraceEventTLS());
        delete static_cast<TrackEventThreadLocalEventSink*>(event_sink);
      });

  return thread_local_event_sink_tls.get();
}

TraceEventDataSource* g_trace_event_data_source_for_testing = nullptr;

// crbug.com/914092: This has to be large enough for DevTools to be able to
// start up and telemetry to start tracing through it before the buffer is
// exhausted.
constexpr size_t kMaxStartupWriterBufferSize = 10 * 1024 * 1024;

}  // namespace

// static
TraceEventDataSource* TraceEventDataSource::GetInstance() {
  static base::NoDestructor<TraceEventDataSource> instance;
  return instance.get();
}

// static
base::ThreadLocalBoolean* TraceEventDataSource::GetThreadIsInTraceEventTLS() {
  static base::NoDestructor<base::ThreadLocalBoolean> thread_is_in_trace_event;
  return thread_is_in_trace_event.get();
}

// static
void TraceEventDataSource::ResetForTesting() {
  if (!g_trace_event_data_source_for_testing)
    return;
  g_trace_event_data_source_for_testing->~TraceEventDataSource();
  new (g_trace_event_data_source_for_testing) TraceEventDataSource;
}

TraceEventDataSource::TraceEventDataSource()
    : DataSourceBase(mojom::kTraceEventDataSourceName),
      disable_interning_(base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kPerfettoDisableInterning)) {
  DCHECK(session_flags_.is_lock_free())
      << "SessionFlags are not atomic! We rely on efficient lock-free look-up "
         "of the session flags when emitting a trace event.";
  g_trace_event_data_source_for_testing = this;
  DETACH_FROM_SEQUENCE(perfetto_sequence_checker_);
}

TraceEventDataSource::~TraceEventDataSource() = default;

void TraceEventDataSource::RegisterStartupHooks() {
  RegisterTracedValueProtoWriter();
}

void TraceEventDataSource::RegisterWithTraceLog() {
  TraceLog::GetInstance()->SetAddTraceEventOverrides(
      &TraceEventDataSource::OnAddTraceEvent,
      &TraceEventDataSource::FlushCurrentThread,
      &TraceEventDataSource::OnUpdateDuration);
  base::AutoLock l(lock_);
  is_enabled_ = true;
}

void TraceEventDataSource::UnregisterFromTraceLog() {
  TraceLog::GetInstance()->SetAddTraceEventOverrides(nullptr, nullptr, nullptr);
  base::AutoLock l(lock_);
  is_enabled_ = false;
  flushing_trace_log_ = false;
  DCHECK(!flush_complete_task_);
}

// static
TrackEventThreadLocalEventSink* TraceEventDataSource::GetOrPrepareEventSink(
    bool thread_will_flush) {
  // Avoid re-entrancy, which can happen during PostTasks (the taskqueue can
  // emit trace events). We discard the events in this case, as any PostTasking
  // to deal with these events later would break the event ordering that the
  // JSON traces rely on to merge 'A'/'B' events, as well as having to deal with
  // updating duration of 'X' events which haven't been added yet.
  if (GetThreadIsInTraceEventTLS()->Get()) {
    return nullptr;
  }

  AutoThreadLocalBoolean thread_is_in_trace_event(GetThreadIsInTraceEventTLS());

  auto* thread_local_event_sink = static_cast<TrackEventThreadLocalEventSink*>(
      ThreadLocalEventSinkSlot()->Get());

  // Make sure the sink was reset since the last tracing session. Normally, it
  // is reset on Flush after the session is disabled. However, it may not have
  // been reset if the current thread doesn't support flushing. In that case, we
  // need to check here that it writes to the right buffer.
  //
  // Because we want to avoid locking for each event, we access |session_flags_|
  // racily. It's OK if we don't see it change to the session immediately. In
  // that case, the first few trace events may get lost, but we will eventually
  // notice that we are writing to the wrong buffer once the change to
  // |session_flags_| has propagated, and reset the sink. Note we will still
  // acquire the |lock_| to safely recreate the sink in
  // CreateThreadLocalEventSink().
  if (thread_local_event_sink) {
    SessionFlags new_session_flags =
        GetInstance()->session_flags_.load(std::memory_order_relaxed);
    if (new_session_flags.session_id != thread_local_event_sink->session_id()) {
      delete thread_local_event_sink;
      thread_local_event_sink = nullptr;
    }
  }

  if (!thread_local_event_sink) {
    thread_local_event_sink =
        GetInstance()->CreateThreadLocalEventSink(thread_will_flush);
    ThreadLocalEventSinkSlot()->Set(thread_local_event_sink);
  }

  return thread_local_event_sink;
}

bool TraceEventDataSource::IsEnabled() {
  base::AutoLock l(lock_);
  return is_enabled_;
}

void TraceEventDataSource::SetupStartupTracing(bool privacy_filtering_enabled) {
  {
    AutoLockWithDeferredTaskPosting lock(lock_);
    // Do not enable startup registry if trace log is being flushed. The
    // previous tracing session has not ended yet.
    if (flushing_trace_log_) {
      return;
    }
    // No need to do anything if startup tracing has already been set,
    // or we know Perfetto has already been setup.
    if (startup_writer_registry_ || producer_) {
      DCHECK(!privacy_filtering_enabled || privacy_filtering_enabled_);
      return;
    }

    privacy_filtering_enabled_ = privacy_filtering_enabled;
    startup_writer_registry_ =
        std::make_unique<perfetto::StartupTraceWriterRegistry>();
    SetStartupTracingFlagsWhileLocked();

    DCHECK(!trace_writer_);
    trace_writer_ = CreateTraceWriterLocked();
  }
  EmitTrackDescriptor();
  RegisterWithTraceLog();
  if (base::SequencedTaskRunnerHandle::IsSet()) {
    OnTaskSchedulerAvailable();
  }
}

void TraceEventDataSource::OnTaskSchedulerAvailable() {
  CHECK(IsTracingInitialized());
  {
    base::AutoLock lock(lock_);
    if (!startup_writer_registry_)
      return;
  }
  startup_tracing_timer_.Start(
      FROM_HERE, startup_tracing_timeout_,
      base::BindOnce(&TraceEventDataSource::StartupTracingTimeoutFired,
                     base::Unretained(this)));
}

void TraceEventDataSource::StartupTracingTimeoutFired() {
  auto task_runner =
      PerfettoTracedProcess::Get()->GetTaskRunner()->GetOrCreateTaskRunner();
  if (!task_runner->RunsTasksInCurrentSequence()) {
    task_runner->PostTask(
        FROM_HERE,
        base::BindOnce(&TraceEventDataSource::StartupTracingTimeoutFired,
                       base::Unretained(this)));
    return;
  }
  DCHECK_CALLED_ON_VALID_SEQUENCE(perfetto_sequence_checker_);
  std::unique_ptr<perfetto::StartupTraceWriterRegistry> registry;
  std::unique_ptr<perfetto::StartupTraceWriter> trace_writer;
  {
    AutoLockWithDeferredTaskPosting lock(lock_);
    if (!startup_writer_registry_) {
      return;
    }
    // Set startup_writer_registry_ to null so that no further writers are
    // created.
    startup_writer_registry_.reset();
    flushing_trace_log_ = true;
    trace_writer = std::move(trace_writer_);
  }
  if (trace_writer) {
    ReturnTraceWriter(std::move(trace_writer));
  }
  auto* trace_log = base::trace_event::TraceLog::GetInstance();
  trace_log->SetDisabled();
  trace_log->Flush(base::BindRepeating(&TraceEventDataSource::OnFlushFinished,
                                       base::Unretained(this)),
                   /*use_worker_thread=*/false);
}

void TraceEventDataSource::IncrementSessionIdOrClearStartupFlagWhileLocked() {
  // Protected by |lock_| for CreateThreadLocalEventSink() and
  // SetStartupTracingFlagsWhileLocked().
  lock_.AssertAcquired();
  SessionFlags flags = session_flags_.load(std::memory_order_relaxed);
  if (flags.is_startup_tracing) {
    // Don't increment the session ID if startup tracing was active for this
    // session. This way, the sinks that were created while startup tracing for
    // the current session won't be cleared away (resetting such sinks could
    // otherwise cause data buffered in their potentially still unbound
    // StartupTraceWriters to be lost).
    flags.is_startup_tracing = false;
  } else {
    flags.session_id++;
  }
  session_flags_.store(flags, std::memory_order_relaxed);
}

void TraceEventDataSource::SetStartupTracingFlagsWhileLocked() {
  // Protected by |lock_| for CreateThreadLocalEventSink() and
  // IncrementSessionIdOrClearStartupFlagWhileLocked().
  lock_.AssertAcquired();
  SessionFlags flags = session_flags_.load(std::memory_order_relaxed);
  flags.is_startup_tracing = true;
  flags.session_id++;
  session_flags_.store(flags, std::memory_order_relaxed);
}

void TraceEventDataSource::OnFlushFinished(
    const scoped_refptr<base::RefCountedString>&,
    bool has_more_events) {
  if (has_more_events) {
    return;
  }

  // Clear the pending task on the tracing service thread.
  DCHECK_CALLED_ON_VALID_SEQUENCE(perfetto_sequence_checker_);
  base::OnceClosure task;
  {
    AutoLockWithDeferredTaskPosting l(lock_);
    // Run any pending start or stop tracing
    // task.
    task = std::move(flush_complete_task_);
    flushing_trace_log_ = false;

    // Increment the session id to make sure that once tracing starts the events
    // are added to a new trace writer that comes from perfetto producer,
    // instead of holding on to the startup registry's writers.
    IncrementSessionIdOrClearStartupFlagWhileLocked();
  }
  if (task) {
    std::move(task).Run();
  }
}

void TraceEventDataSource::StartTracing(
    PerfettoProducer* producer,
    const perfetto::DataSourceConfig& data_source_config) {
  {
    AutoLockWithDeferredTaskPosting l(lock_);
    if (flushing_trace_log_) {
      DCHECK(!flush_complete_task_);
      // Delay start tracing until flush is finished.
      // Unretained is fine here because the producer will be valid till
      // stop tracing is called and at stop this task will be cleared.
      flush_complete_task_ = base::BindOnce(
          &TraceEventDataSource::StartTracingInternal, base::Unretained(this),
          base::Unretained(producer), data_source_config);
      return;
    }
  }
  StartTracingInternal(producer, data_source_config);
}

void TraceEventDataSource::StartTracingInternal(
    PerfettoProducer* producer,
    const perfetto::DataSourceConfig& data_source_config) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(perfetto_sequence_checker_);
  std::unique_ptr<perfetto::StartupTraceWriterRegistry> unbound_writer_registry;
  {
    AutoLockWithDeferredTaskPosting lock(lock_);
    bool should_enable_filtering =
        data_source_config.chrome_config().privacy_filtering_enabled();
    if (should_enable_filtering) {
      CHECK(!startup_writer_registry_ || privacy_filtering_enabled_)
          << "Unexpected StartTracing received when startup tracing is "
             "running.";
    }
    privacy_filtering_enabled_ = should_enable_filtering;

    producer_ = producer;
    target_buffer_ = data_source_config.target_buffer();
    // Reduce lock contention by binding the registry without holding the lock.
    unbound_writer_registry = std::move(startup_writer_registry_);

    IncrementSessionIdOrClearStartupFlagWhileLocked();

    if (!trace_writer_) {
      trace_writer_ = CreateTraceWriterLocked();
    }
  }

  if (unbound_writer_registry) {
    // TODO(oysteine): Investigate why trace events emitted by something in
    // BindStartupTraceWriterRegistry() causes deadlocks.
    AutoThreadLocalBoolean thread_is_in_trace_event(
        GetThreadIsInTraceEventTLS());
    producer->BindStartupTraceWriterRegistry(
        std::move(unbound_writer_registry), data_source_config.target_buffer());
  } else {
    RegisterWithTraceLog();
  }

  // We emit the track/process descriptor another time even if we were
  // previously startup tracing, because the process name may have changed.
  EmitTrackDescriptor();

  auto trace_config =
      TraceConfig(data_source_config.chrome_config().trace_config());
  TraceLog::GetInstance()->SetEnabled(trace_config, TraceLog::RECORDING_MODE);
  ResetHistograms(trace_config);

  if (trace_config.IsCategoryGroupEnabled(
          TRACE_DISABLED_BY_DEFAULT("histogram_samples"))) {
    base::StatisticsRecorder::SetGlobalSampleCallback(
        &TraceEventDataSource::OnMetricsSampleCallback);
  }
  if (trace_config.IsCategoryGroupEnabled(
          TRACE_DISABLED_BY_DEFAULT("user_action_samples"))) {
    auto task_runner = base::GetRecordActionTaskRunner();
    if (task_runner) {
      task_runner->PostTask(
          FROM_HERE, base::Bind([]() {
            base::AddActionCallback(
                TraceEventDataSource::GetInstance()->user_action_callback_);
          }));
    }
  }
}

void TraceEventDataSource::StopTracing(
    base::OnceClosure stop_complete_callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(perfetto_sequence_checker_);
  stop_complete_callback_ = std::move(stop_complete_callback);

  auto on_tracing_stopped_callback =
      [](TraceEventDataSource* data_source,
         const scoped_refptr<base::RefCountedString>&, bool has_more_events) {
        if (has_more_events) {
          return;
        }

        data_source->UnregisterFromTraceLog();

        if (data_source->stop_complete_callback_) {
          std::move(data_source->stop_complete_callback_).Run();
        }
      };

  bool was_enabled = TraceLog::GetInstance()->IsEnabled();
  if (was_enabled) {
    // Write metadata events etc.
    LogHistograms();
    TraceLog::GetInstance()->SetDisabled();
  }

  std::unique_ptr<perfetto::StartupTraceWriter> trace_writer;
  {
    AutoLockWithDeferredTaskPosting lock(lock_);
    if (flush_complete_task_) {
      DCHECK(!producer_);
      // Skip start tracing task at this point if we still have not flushed
      // trace log. We wouldn't be replacing a |flush_complete_task_| that is
      // stop tracing callback task at any point, since perfetto will wait for
      // the callback before starting next session.
      flush_complete_task_ =
          base::BindOnce(std::move(on_tracing_stopped_callback), this,
                         scoped_refptr<base::RefCountedString>(), false);
      return;
    }
    // Prevent recreation of ThreadLocalEventSinks after flush.
    DCHECK(producer_);
    producer_ = nullptr;
    target_buffer_ = 0;
    flushing_trace_log_ = was_enabled;
    trace_writer = std::move(trace_writer_);
  }
  if (trace_writer) {
    ReturnTraceWriter(std::move(trace_writer));
  }

  if (was_enabled) {
    // TraceLog::SetDisabled will cause metadata events to be written; make
    // sure we flush the TraceWriter for this thread (TraceLog will only call
    // TraceEventDataSource::FlushCurrentThread for threads with a MessageLoop).
    // TODO(eseckler): Flush all worker threads.
    // TODO(oysteine): The perfetto service itself should be able to recover
    // unreturned chunks so technically this can go away at some point, but
    // seems needed for now.
    FlushCurrentThread();

    // Flush the remaining threads via TraceLog. We call CancelTracing because
    // we don't want/need TraceLog to do any of its own JSON serialization.
    TraceLog::GetInstance()->CancelTracing(base::BindRepeating(
        on_tracing_stopped_callback, base::Unretained(this)));
  } else {
    on_tracing_stopped_callback(this, scoped_refptr<base::RefCountedString>(),
                                false);
  }

  base::StatisticsRecorder::SetGlobalSampleCallback(nullptr);
  auto task_runner = base::GetRecordActionTaskRunner();
  if (task_runner) {
    task_runner->PostTask(
        FROM_HERE, base::Bind([]() {
          base::RemoveActionCallback(
              TraceEventDataSource::GetInstance()->user_action_callback_);
        }));
  }
}

void TraceEventDataSource::LogHistogram(base::HistogramBase* histogram) {
  if (!histogram) {
    return;
  }
  auto samples = histogram->SnapshotSamples();
  base::Pickle pickle;
  samples->Serialize(&pickle);
  std::string buckets;
  base::Base64Encode(
      std::string(static_cast<const char*>(pickle.data()), pickle.size()),
      &buckets);
  TRACE_EVENT_INSTANT2("benchmark", "UMAHistogramSamples",
                       TRACE_EVENT_SCOPE_PROCESS, "name",
                       histogram->histogram_name(), "buckets", buckets);
}

void TraceEventDataSource::ResetHistograms(const TraceConfig& trace_config) {
  histograms_.clear();
  for (const std::string& histogram_name : trace_config.histogram_names()) {
    histograms_.push_back(histogram_name);
    LogHistogram(base::StatisticsRecorder::FindHistogram(histogram_name));
  }
}

void TraceEventDataSource::LogHistograms() {
  for (const std::string& histogram_name : histograms_) {
    LogHistogram(base::StatisticsRecorder::FindHistogram(histogram_name));
  }
}

void TraceEventDataSource::Flush(
    base::RepeatingClosure flush_complete_callback) {
  DCHECK(TraceLog::GetInstance()->IsEnabled());
  TraceLog::GetInstance()->Flush(base::BindRepeating(
      [](base::RepeatingClosure flush_complete_callback,
         const scoped_refptr<base::RefCountedString>&, bool has_more_events) {
        if (has_more_events) {
          return;
        }

        flush_complete_callback.Run();
      },
      std::move(flush_complete_callback)));
}

void TraceEventDataSource::ClearIncrementalState() {
  TrackEventThreadLocalEventSink::ClearIncrementalState();
  EmitTrackDescriptor();
}

std::unique_ptr<perfetto::StartupTraceWriter>
TraceEventDataSource::CreateTraceWriterLocked() {
  lock_.AssertAcquired();
  // |startup_writer_registry_| only exists during startup tracing before we
  // connect to the service. |producer_| is reset when tracing is
  // stopped.
  std::unique_ptr<perfetto::StartupTraceWriter> trace_writer;
  if (startup_writer_registry_) {
    // Chromium uses BufferExhaustedPolicy::kDrop to avoid stalling trace
    // writers when the chunks in the SMB are exhausted. Stalling could
    // otherwise lead to deadlocks in chromium, because a stalled mojo IPC
    // thread could prevent CommitRequest messages from reaching the perfetto
    // service.
    auto buffer_exhausted_policy = perfetto::BufferExhaustedPolicy::kDrop;
    trace_writer = startup_writer_registry_->CreateUnboundTraceWriter(
        buffer_exhausted_policy, kMaxStartupWriterBufferSize);
  } else if (producer_) {
    trace_writer = std::make_unique<perfetto::StartupTraceWriter>(
        producer_->CreateTraceWriter(target_buffer_));
  }
  return trace_writer;
}

TrackEventThreadLocalEventSink*
TraceEventDataSource::CreateThreadLocalEventSink(bool thread_will_flush) {
  AutoLockWithDeferredTaskPosting lock(lock_);
  uint32_t session_id =
      session_flags_.load(std::memory_order_relaxed).session_id;

  auto trace_writer = CreateTraceWriterLocked();
  if (!trace_writer) {
    return nullptr;
  }

  return new TrackEventThreadLocalEventSink(std::move(trace_writer), session_id,
                                            disable_interning_,
                                            privacy_filtering_enabled_);
}

// static
void TraceEventDataSource::OnAddTraceEvent(
    TraceEvent* trace_event,
    bool thread_will_flush,
    base::trace_event::TraceEventHandle* handle) {
  OnAddTraceEvent(trace_event, thread_will_flush, handle,
                  [](perfetto::EventContext) {});
}

// static
void TraceEventDataSource::OnUpdateDuration(
    const unsigned char* category_group_enabled,
    const char* name,
    base::trace_event::TraceEventHandle handle,
    int thread_id,
    bool explicit_timestamps,
    const base::TimeTicks& now,
    const base::ThreadTicks& thread_now,
    base::trace_event::ThreadInstructionCount thread_instruction_now) {
  if (GetThreadIsInTraceEventTLS()->Get()) {
    return;
  }

  AutoThreadLocalBoolean thread_is_in_trace_event(GetThreadIsInTraceEventTLS());

  auto* thread_local_event_sink = static_cast<TrackEventThreadLocalEventSink*>(
      ThreadLocalEventSinkSlot()->Get());
  if (thread_local_event_sink) {
    thread_local_event_sink->UpdateDuration(
        category_group_enabled, name, handle, thread_id, explicit_timestamps,
        now, thread_now, thread_instruction_now);
  }
}

// static
void TraceEventDataSource::FlushCurrentThread() {
  auto* thread_local_event_sink = static_cast<TrackEventThreadLocalEventSink*>(
      ThreadLocalEventSinkSlot()->Get());
  if (thread_local_event_sink) {
    // Prevent any events from being emitted while we're deleting
    // the sink (like from the TraceWriter being PostTask'ed for deletion).
    AutoThreadLocalBoolean thread_is_in_trace_event(
        GetThreadIsInTraceEventTLS());
    thread_local_event_sink->Flush();
    // TODO(oysteine): To support flushing while still recording, this needs to
    // be changed to not destruct the TLS object as that will emit any
    // uncompleted _COMPLETE events on the stack.
    delete thread_local_event_sink;
    ThreadLocalEventSinkSlot()->Set(nullptr);
  }
}

// static
void TraceEventDataSource::OnMetricsSampleCallback(
    const char* histogram_name,
    uint64_t name_hash,
    base::HistogramBase::Sample sample) {
  // TODO(oysteine): Write an interned histogram name during local dev tracing
  // when we're less space constrained.
  TRACE_EVENT(TRACE_DISABLED_BY_DEFAULT("histogram_samples"), "HistogramSample",
              [&](perfetto::EventContext ctx) {
                perfetto::protos::pbzero::ChromeHistogramSample* new_sample =
                    ctx.event()->set_chrome_histogram_sample();
                new_sample->set_name_hash(name_hash);
                new_sample->set_sample(sample);
              });
}

void TraceEventDataSource::OnUserActionSampleCallback(
    const std::string& action) {
  TRACE_EVENT(TRACE_DISABLED_BY_DEFAULT("user_action_samples"), "UserAction",
              [&](perfetto::EventContext ctx) {
                perfetto::protos::pbzero::ChromeUserEvent* new_sample =
                    ctx.event()->set_chrome_user_event();
                // TODO(ssid): Set action string in non filtered mode.
                new_sample->set_action_hash(base::HashMetricName(action));
              });
}

void TraceEventDataSource::ReturnTraceWriter(
    std::unique_ptr<perfetto::StartupTraceWriter> trace_writer) {
  {
    // Prevent concurrent binding of the registry.
    AutoLockWithDeferredTaskPosting lock(lock_);

    // If we don't have a task runner yet, we must be attempting to return a
    // writer before the (very first) registry was bound. We cannot create the
    // task runner safely in this case, because the thread pool may not have
    // been brought up yet.
    if (!PerfettoTracedProcess::GetTaskRunner()->HasTaskRunner()) {
      DCHECK(startup_writer_registry_);
      // It's safe to call ReturnToRegistry on the current sequence, as it won't
      // destroy the writer since the registry was not bound yet. Will keep
      // |trace_writer| alive until the registry is bound later.
      perfetto::StartupTraceWriter::ReturnToRegistry(std::move(trace_writer));
      return;
    }
  }

  // Return the TraceWriter on the sequence that Perfetto runs on. Needed as the
  // ThreadLocalEventSink gets deleted on thread shutdown and we can't safely
  // call TaskRunnerHandle::Get() at that point (which can happen as the
  // TraceWriter destructor might make a Mojo call and trigger it).
  auto* trace_writer_raw = trace_writer.release();
  ANNOTATE_LEAKING_OBJECT_PTR(trace_writer_raw);
  PerfettoTracedProcess::GetTaskRunner()->GetOrCreateTaskRunner()->PostTask(
      FROM_HERE,
      base::BindOnce(
          // Pass writer as raw pointer so that we leak it if task posting fails
          // (during shutdown).
          [](perfetto::StartupTraceWriter* trace_writer) {
            // May destroy |trace_writer|. If the writer is still unbound, the
            // registry will keep it alive until it was bound and its buffered
            // data was copied. This ensures that we don't lose data from
            // threads that are shut down during startup.
            perfetto::StartupTraceWriter::ReturnToRegistry(
                base::WrapUnique<perfetto::StartupTraceWriter>(trace_writer));
          },
          trace_writer_raw));
}

void TraceEventDataSource::EmitTrackDescriptor() {
  AutoLockWithDeferredTaskPosting lock(lock_);

  int process_id = TraceLog::GetInstance()->process_id();
  if (process_id == base::kNullProcessId) {
    // Do not emit descriptor without process id.
    return;
  }

  if (!trace_writer_) {
    return;
  }

  std::string process_name = TraceLog::GetInstance()->process_name();

  TracePacketHandle trace_packet = trace_writer_->NewTracePacket();

  trace_packet->set_sequence_flags(
      perfetto::protos::pbzero::TracePacket::SEQ_INCREMENTAL_STATE_CLEARED);
  trace_packet->set_timestamp(
      TRACE_TIME_TICKS_NOW().since_origin().InNanoseconds());
  trace_packet->set_timestamp_clock_id(kTraceClockId);

  TrackDescriptor* track_descriptor = trace_packet->set_track_descriptor();
  auto process_track = perfetto::ProcessTrack::Current();

  // TODO(eseckler): Call process_track.Serialize() here instead once the
  // client lib also fills in the ProcessDescriptor's process_name, gets the
  // correct pid from Chrome, and supports privacy filtering.
  track_descriptor->set_uuid(process_track.uuid);
  PERFETTO_DCHECK(!process_track.parent_uuid);

  ProcessDescriptor* process = track_descriptor->set_process();
  process->set_pid(process_id);
  if (!privacy_filtering_enabled_ && !process_name.empty()) {
    process->set_process_name(process_name);
  }

  ChromeProcessDescriptor* chrome_process =
      track_descriptor->set_chrome_process();
  auto process_type = GetProcessType(process_name);
  if (process_type != ChromeProcessDescriptor::PROCESS_UNSPECIFIED) {
    chrome_process->set_process_type(process_type);
  }

  // TODO(eseckler): Set other fields on |chrome_process|.

  trace_packet = TracePacketHandle();
  trace_writer_->Flush();
}

}  // namespace tracing
