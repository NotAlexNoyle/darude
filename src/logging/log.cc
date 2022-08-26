// Copyright 2011 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/logging/log.h"

#include <atomic>
#include <cstdarg>
#include <memory>
#include <sstream>

#include "include/v8-locker.h"
#include "src/api/api-inl.h"
#include "src/base/platform/mutex.h"
#include "src/base/platform/platform.h"
#include "src/base/platform/wrappers.h"
#include "src/builtins/profile-data-reader.h"
#include "src/codegen/bailout-reason.h"
#include "src/codegen/macro-assembler.h"
#include "src/codegen/source-position-table.h"
#include "src/deoptimizer/deoptimizer.h"
#include "src/diagnostics/perf-jit.h"
#include "src/execution/isolate.h"
#include "src/execution/v8threads.h"
#include "src/execution/vm-state-inl.h"
#include "src/handles/global-handles.h"
#include "src/heap/combined-heap.h"
#include "src/heap/heap-inl.h"
#include "src/init/bootstrapper.h"
#include "src/interpreter/bytecodes.h"
#include "src/interpreter/interpreter.h"
#include "src/libsampler/sampler.h"
#include "src/logging/code-events.h"
#include "src/logging/counters.h"
#include "src/logging/log-file.h"
#include "src/logging/log-inl.h"
#include "src/objects/api-callbacks.h"
#include "src/objects/code-kind.h"
#include "src/objects/code.h"
#include "src/profiler/tick-sample.h"
#include "src/snapshot/embedded/embedded-data.h"
#include "src/strings/string-stream.h"
#include "src/strings/unicode-inl.h"
#include "src/tracing/tracing-category-observer.h"
#include "src/utils/memcopy.h"
#include "src/utils/version.h"

#ifdef ENABLE_GDB_JIT_INTERFACE
#include "src/diagnostics/gdb-jit.h"
#endif  // ENABLE_GDB_JIT_INTERFACE

#if V8_ENABLE_WEBASSEMBLY
#include "src/wasm/wasm-code-manager.h"
#include "src/wasm/wasm-engine.h"
#include "src/wasm/wasm-objects-inl.h"
#endif  // V8_ENABLE_WEBASSEMBLY

#if V8_OS_WIN
#if defined(V8_ENABLE_ETW_STACK_WALKING)
#include "src/diagnostics/etw-jit-win.h"
#endif
#endif  // V8_OS_WIN

namespace v8 {
namespace internal {

static const char* kLogEventsNames[] = {
#define DECLARE_EVENT(ignore1, name) name,
    LOG_EVENT_LIST(DECLARE_EVENT)
#undef DECLARE_EVENT
};
static const char* kCodeTagNames[] = {
#define DECLARE_EVENT(ignore1, name) #name,
    CODE_TYPE_LIST(DECLARE_EVENT)
#undef DECLARE_EVENT
};

std::ostream& operator<<(std::ostream& os, LogEventListener::CodeTag tag) {
  os << kCodeTagNames[static_cast<int>(tag)];
  return os;
}
std::ostream& operator<<(std::ostream& os, LogEventListener::Event event) {
  os << kLogEventsNames[static_cast<int>(event)];
  return os;
}

namespace {

v8::CodeEventType GetCodeEventTypeForTag(LogEventListener::CodeTag tag) {
  switch (tag) {
    case LogEventListener::CodeTag::kLength:
    // Manually create this switch, since v8::CodeEventType is API expose and
    // cannot be easily modified.
    case LogEventListener::CodeTag::kBuiltin:
      return v8::CodeEventType::kBuiltinType;
    case LogEventListener::CodeTag::kCallback:
      return v8::CodeEventType::kCallbackType;
    case LogEventListener::CodeTag::kEval:
      return v8::CodeEventType::kEvalType;
    case LogEventListener::CodeTag::kNativeFunction:
    case LogEventListener::CodeTag::kFunction:
      return v8::CodeEventType::kFunctionType;
    case LogEventListener::CodeTag::kHandler:
      return v8::CodeEventType::kHandlerType;
    case LogEventListener::CodeTag::kBytecodeHandler:
      return v8::CodeEventType::kBytecodeHandlerType;
    case LogEventListener::CodeTag::kRegExp:
      return v8::CodeEventType::kRegExpType;
    case LogEventListener::CodeTag::kNativeScript:
    case LogEventListener::CodeTag::kScript:
      return v8::CodeEventType::kScriptType;
    case LogEventListener::CodeTag::kStub:
      return v8::CodeEventType::kStubType;
  }
  UNREACHABLE();
}

#define CALL_CODE_EVENT_HANDLER(Call) \
  if (listener_) {                    \
    listener_->Call;                  \
  } else {                            \
    PROFILE(isolate_, Call);          \
  }

const char* ComputeMarker(SharedFunctionInfo shared, AbstractCode code) {
  PtrComprCageBase cage_base = GetPtrComprCageBase(shared);
  CodeKind kind = code.kind(cage_base);
  // We record interpreter trampoline builtin copies as having the
  // "interpreted" marker.
  if (FLAG_interpreted_frames_native_stack && kind == CodeKind::BUILTIN &&
      !code.is_off_heap_trampoline(cage_base)) {
    DCHECK_EQ(code.builtin_id(cage_base), Builtin::kInterpreterEntryTrampoline);
    kind = CodeKind::INTERPRETED_FUNCTION;
  }
  if (shared.optimization_disabled() &&
      kind == CodeKind::INTERPRETED_FUNCTION) {
    return "";
  }
  return CodeKindToMarker(kind);
}

#if V8_ENABLE_WEBASSEMBLY
const char* ComputeMarker(const wasm::WasmCode* code) {
  switch (code->kind()) {
    case wasm::WasmCode::kWasmFunction:
      return code->is_liftoff() ? "" : "*";
    default:
      return "";
  }
}
#endif  // V8_ENABLE_WEBASSEMBLY

}  // namespace

class CodeEventLogger::NameBuffer {
 public:
  NameBuffer() { Reset(); }

  void Reset() { utf8_pos_ = 0; }

  void Init(CodeTag tag) {
    Reset();
    AppendBytes(kCodeTagNames[static_cast<int>(tag)]);
    AppendByte(':');
  }

  void AppendName(Name name) {
    if (name.IsString()) {
      AppendString(String::cast(name));
    } else {
      Symbol symbol = Symbol::cast(name);
      AppendBytes("symbol(");
      if (!symbol.description().IsUndefined()) {
        AppendBytes("\"");
        AppendString(String::cast(symbol.description()));
        AppendBytes("\" ");
      }
      AppendBytes("hash ");
      AppendHex(symbol.hash());
      AppendByte(')');
    }
  }

  void AppendString(String str) {
    if (str.is_null()) return;
    int length = 0;
    std::unique_ptr<char[]> c_str =
        str.ToCString(DISALLOW_NULLS, ROBUST_STRING_TRAVERSAL, &length);
    AppendBytes(c_str.get(), length);
  }

  void AppendBytes(const char* bytes, int size) {
    size = std::min(size, kUtf8BufferSize - utf8_pos_);
    MemCopy(utf8_buffer_ + utf8_pos_, bytes, size);
    utf8_pos_ += size;
  }

  void AppendBytes(const char* bytes) {
    size_t len = strlen(bytes);
    DCHECK_GE(kMaxInt, len);
    AppendBytes(bytes, static_cast<int>(len));
  }

  void AppendByte(char c) {
    if (utf8_pos_ >= kUtf8BufferSize) return;
    utf8_buffer_[utf8_pos_++] = c;
  }

  void AppendInt(int n) {
    int space = kUtf8BufferSize - utf8_pos_;
    if (space <= 0) return;
    base::Vector<char> buffer(utf8_buffer_ + utf8_pos_, space);
    int size = SNPrintF(buffer, "%d", n);
    if (size > 0 && utf8_pos_ + size <= kUtf8BufferSize) {
      utf8_pos_ += size;
    }
  }

  void AppendHex(uint32_t n) {
    int space = kUtf8BufferSize - utf8_pos_;
    if (space <= 0) return;
    base::Vector<char> buffer(utf8_buffer_ + utf8_pos_, space);
    int size = SNPrintF(buffer, "%x", n);
    if (size > 0 && utf8_pos_ + size <= kUtf8BufferSize) {
      utf8_pos_ += size;
    }
  }

  const char* get() { return utf8_buffer_; }
  int size() const { return utf8_pos_; }

 private:
  static const int kUtf8BufferSize = 4096;
  static const int kUtf16BufferSize = kUtf8BufferSize;

  int utf8_pos_;
  char utf8_buffer_[kUtf8BufferSize];
};

CodeEventLogger::CodeEventLogger(Isolate* isolate)
    : isolate_(isolate), name_buffer_(std::make_unique<NameBuffer>()) {}

CodeEventLogger::~CodeEventLogger() = default;

void CodeEventLogger::CodeCreateEvent(CodeTag tag, Handle<AbstractCode> code,
                                      const char* comment) {
  DCHECK(is_listening_to_code_events());
  name_buffer_->Init(tag);
  name_buffer_->AppendBytes(comment);
  LogRecordedBuffer(code, MaybeHandle<SharedFunctionInfo>(),
                    name_buffer_->get(), name_buffer_->size());
}

void CodeEventLogger::CodeCreateEvent(CodeTag tag, Handle<AbstractCode> code,
                                      Handle<Name> name) {
  DCHECK(is_listening_to_code_events());
  name_buffer_->Init(tag);
  name_buffer_->AppendName(*name);
  LogRecordedBuffer(code, MaybeHandle<SharedFunctionInfo>(),
                    name_buffer_->get(), name_buffer_->size());
}

void CodeEventLogger::CodeCreateEvent(CodeTag tag, Handle<AbstractCode> code,
                                      Handle<SharedFunctionInfo> shared,
                                      Handle<Name> script_name) {
  DCHECK(is_listening_to_code_events());
  name_buffer_->Init(tag);
  name_buffer_->AppendBytes(ComputeMarker(*shared, *code));
  name_buffer_->AppendByte(' ');
  name_buffer_->AppendName(*script_name);
  LogRecordedBuffer(code, shared, name_buffer_->get(), name_buffer_->size());
}

void CodeEventLogger::CodeCreateEvent(CodeTag tag, Handle<AbstractCode> code,
                                      Handle<SharedFunctionInfo> shared,
                                      Handle<Name> script_name, int line,
                                      int column) {
  DCHECK(is_listening_to_code_events());
  name_buffer_->Init(tag);
  name_buffer_->AppendBytes(ComputeMarker(*shared, *code));
  name_buffer_->AppendBytes(shared->DebugNameCStr().get());
  name_buffer_->AppendByte(' ');
  if (script_name->IsString()) {
    name_buffer_->AppendString(String::cast(*script_name));
  } else {
    name_buffer_->AppendBytes("symbol(hash ");
    name_buffer_->AppendHex(Name::cast(*script_name).hash());
    name_buffer_->AppendByte(')');
  }
  name_buffer_->AppendByte(':');
  name_buffer_->AppendInt(line);
  name_buffer_->AppendByte(':');
  name_buffer_->AppendInt(column);
  LogRecordedBuffer(code, shared, name_buffer_->get(), name_buffer_->size());
}

#if V8_ENABLE_WEBASSEMBLY
void CodeEventLogger::CodeCreateEvent(CodeTag tag, const wasm::WasmCode* code,
                                      wasm::WasmName name,
                                      const char* source_url,
                                      int /*code_offset*/, int /*script_id*/) {
  DCHECK(is_listening_to_code_events());
  name_buffer_->Init(tag);
  DCHECK(!name.empty());
  name_buffer_->AppendBytes(name.begin(), name.length());
  name_buffer_->AppendByte('-');
  if (code->IsAnonymous()) {
    name_buffer_->AppendBytes("<anonymous>");
  } else {
    name_buffer_->AppendInt(code->index());
  }
  name_buffer_->AppendByte('-');
  name_buffer_->AppendBytes(ExecutionTierToString(code->tier()));
  LogRecordedBuffer(code, name_buffer_->get(), name_buffer_->size());
}
#endif  // V8_ENABLE_WEBASSEMBLY

void CodeEventLogger::RegExpCodeCreateEvent(Handle<AbstractCode> code,
                                            Handle<String> source) {
  DCHECK(is_listening_to_code_events());
  name_buffer_->Init(LogEventListener::CodeTag::kRegExp);
  name_buffer_->AppendString(*source);
  LogRecordedBuffer(code, MaybeHandle<SharedFunctionInfo>(),
                    name_buffer_->get(), name_buffer_->size());
}

// Linux perf tool logging support.
#if V8_OS_LINUX
class LinuxPerfBasicLogger : public CodeEventLogger {
 public:
  explicit LinuxPerfBasicLogger(Isolate* isolate);
  ~LinuxPerfBasicLogger() override;

  void CodeMoveEvent(AbstractCode from, AbstractCode to) override {}
  void CodeDisableOptEvent(Handle<AbstractCode> code,
                           Handle<SharedFunctionInfo> shared) override {}

 private:
  void LogRecordedBuffer(Handle<AbstractCode> code,
                         MaybeHandle<SharedFunctionInfo> maybe_shared,
                         const char* name, int length) override;
#if V8_ENABLE_WEBASSEMBLY
  void LogRecordedBuffer(const wasm::WasmCode* code, const char* name,
                         int length) override;
#endif  // V8_ENABLE_WEBASSEMBLY
  void WriteLogRecordedBuffer(uintptr_t address, int size, const char* name,
                              int name_length);

  // Extension added to V8 log file name to get the low-level log name.
  static const char kFilenameFormatString[];
  static const int kFilenameBufferPadding;

  // Per-process singleton file. We assume that there is one main isolate
  // to determine when it goes away, we keep the reference count.
  static base::LazyRecursiveMutex file_mutex_;
  static FILE* perf_output_handle_;
  static uint64_t reference_count_;
};

const char LinuxPerfBasicLogger::kFilenameFormatString[] = "/tmp/perf-%d.map";
// Extra space for the PID in the filename
const int LinuxPerfBasicLogger::kFilenameBufferPadding = 16;

base::LazyRecursiveMutex LinuxPerfBasicLogger::file_mutex_;
// The following static variables are protected by
// LinuxPerfBasicLogger::file_mutex_.
uint64_t LinuxPerfBasicLogger::reference_count_ = 0;
FILE* LinuxPerfBasicLogger::perf_output_handle_ = nullptr;

LinuxPerfBasicLogger::LinuxPerfBasicLogger(Isolate* isolate)
    : CodeEventLogger(isolate) {
  base::LockGuard<base::RecursiveMutex> guard_file(file_mutex_.Pointer());
  int process_id_ = base::OS::GetCurrentProcessId();
  reference_count_++;
  // If this is the first logger, open the file.
  if (reference_count_ == 1) {
    CHECK_NULL(perf_output_handle_);
    // Open the perf JIT dump file.
    int bufferSize = sizeof(kFilenameFormatString) + kFilenameBufferPadding;
    base::ScopedVector<char> perf_dump_name(bufferSize);
    int size = SNPrintF(perf_dump_name, kFilenameFormatString, process_id_);
    CHECK_NE(size, -1);
    perf_output_handle_ =
        base::OS::FOpen(perf_dump_name.begin(), base::OS::LogFileOpenMode);
    CHECK_NOT_NULL(perf_output_handle_);
    setvbuf(perf_output_handle_, nullptr, _IOLBF, 0);
  }
}

LinuxPerfBasicLogger::~LinuxPerfBasicLogger() {
  base::LockGuard<base::RecursiveMutex> guard_file(file_mutex_.Pointer());
  reference_count_--;

  // If this was the last logger, close the file.
  if (reference_count_ == 0) {
    CHECK_NOT_NULL(perf_output_handle_);
    base::Fclose(perf_output_handle_);
    perf_output_handle_ = nullptr;
  }
}

void LinuxPerfBasicLogger::WriteLogRecordedBuffer(uintptr_t address, int size,
                                                  const char* name,
                                                  int name_length) {
  // Linux perf expects hex literals without a leading 0x, while some
  // implementations of printf might prepend one when using the %p format
  // for pointers, leading to wrongly formatted JIT symbols maps.
  //
  // Instead, we use V8PRIxPTR format string and cast pointer to uintpr_t,
  // so that we have control over the exact output format.
  base::OS::FPrint(perf_output_handle_, "%" V8PRIxPTR " %x %.*s\n", address,
                   size, name_length, name);
}

void LinuxPerfBasicLogger::LogRecordedBuffer(Handle<AbstractCode> code,
                                             MaybeHandle<SharedFunctionInfo>,
                                             const char* name, int length) {
  PtrComprCageBase cage_base(isolate_);
  if (FLAG_perf_basic_prof_only_functions &&
      CodeKindIsBuiltinOrJSFunction(code->kind(cage_base))) {
    return;
  }

  WriteLogRecordedBuffer(
      static_cast<uintptr_t>(code->InstructionStart(cage_base)),
      code->InstructionSize(cage_base), name, length);
}

#if V8_ENABLE_WEBASSEMBLY
void LinuxPerfBasicLogger::LogRecordedBuffer(const wasm::WasmCode* code,
                                             const char* name, int length) {
  WriteLogRecordedBuffer(static_cast<uintptr_t>(code->instruction_start()),
                         code->instructions().length(), name, length);
}
#endif  // V8_ENABLE_WEBASSEMBLY
#endif  // V8_OS_LINUX

// External LogEventListener
ExternalLogEventListener::ExternalLogEventListener(Isolate* isolate)
    : is_listening_(false), isolate_(isolate), code_event_handler_(nullptr) {}

ExternalLogEventListener::~ExternalLogEventListener() {
  if (is_listening_) {
    StopListening();
  }
}

void ExternalLogEventListener::LogExistingCode() {
  HandleScope scope(isolate_);
  ExistingCodeLogger logger(isolate_, this);
  logger.LogBuiltins();
  logger.LogCodeObjects();
  logger.LogCompiledFunctions();
}

void ExternalLogEventListener::StartListening(
    CodeEventHandler* code_event_handler) {
  if (is_listening_ || code_event_handler == nullptr) {
    return;
  }
  code_event_handler_ = code_event_handler;
  is_listening_ = isolate_->logger()->AddListener(this);
  if (is_listening_) {
    LogExistingCode();
  }
}

void ExternalLogEventListener::StopListening() {
  if (!is_listening_) {
    return;
  }

  isolate_->logger()->RemoveListener(this);
  is_listening_ = false;
}

void ExternalLogEventListener::CodeCreateEvent(CodeTag tag,
                                               Handle<AbstractCode> code,
                                               const char* comment) {
  PtrComprCageBase cage_base(isolate_);
  CodeEvent code_event;
  code_event.code_start_address =
      static_cast<uintptr_t>(code->InstructionStart(cage_base));
  code_event.code_size = static_cast<size_t>(code->InstructionSize(cage_base));
  code_event.function_name = isolate_->factory()->empty_string();
  code_event.script_name = isolate_->factory()->empty_string();
  code_event.script_line = 0;
  code_event.script_column = 0;
  code_event.code_type = GetCodeEventTypeForTag(tag);
  code_event.comment = comment;

  code_event_handler_->Handle(reinterpret_cast<v8::CodeEvent*>(&code_event));
}

void ExternalLogEventListener::CodeCreateEvent(CodeTag tag,
                                               Handle<AbstractCode> code,
                                               Handle<Name> name) {
  Handle<String> name_string =
      Name::ToFunctionName(isolate_, name).ToHandleChecked();

  PtrComprCageBase cage_base(isolate_);
  CodeEvent code_event;
  code_event.code_start_address =
      static_cast<uintptr_t>(code->InstructionStart(cage_base));
  code_event.code_size = static_cast<size_t>(code->InstructionSize(cage_base));
  code_event.function_name = name_string;
  code_event.script_name = isolate_->factory()->empty_string();
  code_event.script_line = 0;
  code_event.script_column = 0;
  code_event.code_type = GetCodeEventTypeForTag(tag);
  code_event.comment = "";

  code_event_handler_->Handle(reinterpret_cast<v8::CodeEvent*>(&code_event));
}

void ExternalLogEventListener::CodeCreateEvent(
    CodeTag tag, Handle<AbstractCode> code, Handle<SharedFunctionInfo> shared,
    Handle<Name> name) {
  Handle<String> name_string =
      Name::ToFunctionName(isolate_, name).ToHandleChecked();

  PtrComprCageBase cage_base(isolate_);
  CodeEvent code_event;
  code_event.code_start_address =
      static_cast<uintptr_t>(code->InstructionStart(cage_base));
  code_event.code_size = static_cast<size_t>(code->InstructionSize(cage_base));
  code_event.function_name = name_string;
  code_event.script_name = isolate_->factory()->empty_string();
  code_event.script_line = 0;
  code_event.script_column = 0;
  code_event.code_type = GetCodeEventTypeForTag(tag);
  code_event.comment = "";

  code_event_handler_->Handle(reinterpret_cast<v8::CodeEvent*>(&code_event));
}

void ExternalLogEventListener::CodeCreateEvent(
    CodeTag tag, Handle<AbstractCode> code, Handle<SharedFunctionInfo> shared,
    Handle<Name> source, int line, int column) {
  Handle<String> name_string =
      Name::ToFunctionName(isolate_, handle(shared->Name(), isolate_))
          .ToHandleChecked();
  Handle<String> source_string =
      Name::ToFunctionName(isolate_, source).ToHandleChecked();

  PtrComprCageBase cage_base(isolate_);
  CodeEvent code_event;
  code_event.code_start_address =
      static_cast<uintptr_t>(code->InstructionStart(cage_base));
  code_event.code_size = static_cast<size_t>(code->InstructionSize(cage_base));
  code_event.function_name = name_string;
  code_event.script_name = source_string;
  code_event.script_line = line;
  code_event.script_column = column;
  code_event.code_type = GetCodeEventTypeForTag(tag);
  code_event.comment = "";

  code_event_handler_->Handle(reinterpret_cast<v8::CodeEvent*>(&code_event));
}

#if V8_ENABLE_WEBASSEMBLY
void ExternalLogEventListener::CodeCreateEvent(CodeTag tag,
                                               const wasm::WasmCode* code,
                                               wasm::WasmName name,
                                               const char* source_url,
                                               int code_offset, int script_id) {
  // TODO(mmarchini): handle later
}
#endif  // V8_ENABLE_WEBASSEMBLY

void ExternalLogEventListener::RegExpCodeCreateEvent(Handle<AbstractCode> code,
                                                     Handle<String> source) {
  PtrComprCageBase cage_base(isolate_);
  CodeEvent code_event;
  code_event.code_start_address =
      static_cast<uintptr_t>(code->InstructionStart(cage_base));
  code_event.code_size = static_cast<size_t>(code->InstructionSize(cage_base));
  code_event.function_name = source;
  code_event.script_name = isolate_->factory()->empty_string();
  code_event.script_line = 0;
  code_event.script_column = 0;
  code_event.code_type =
      GetCodeEventTypeForTag(LogEventListener::CodeTag::kRegExp);
  code_event.comment = "";

  code_event_handler_->Handle(reinterpret_cast<v8::CodeEvent*>(&code_event));
}

void ExternalLogEventListener::CodeMoveEvent(AbstractCode from,
                                             AbstractCode to) {
  PtrComprCageBase cage_base(isolate_);
  CodeEvent code_event;
  code_event.previous_code_start_address =
      static_cast<uintptr_t>(from.InstructionStart(cage_base));
  code_event.code_start_address =
      static_cast<uintptr_t>(to.InstructionStart(cage_base));
  code_event.code_size = static_cast<size_t>(to.InstructionSize(cage_base));
  code_event.function_name = isolate_->factory()->empty_string();
  code_event.script_name = isolate_->factory()->empty_string();
  code_event.script_line = 0;
  code_event.script_column = 0;
  code_event.code_type = v8::CodeEventType::kRelocationType;
  code_event.comment = "";

  code_event_handler_->Handle(reinterpret_cast<v8::CodeEvent*>(&code_event));
}

// Low-level logging support.
class LowLevelLogger : public CodeEventLogger {
 public:
  LowLevelLogger(Isolate* isolate, const char* file_name);
  ~LowLevelLogger() override;

  void CodeMoveEvent(AbstractCode from, AbstractCode to) override;
  void CodeDisableOptEvent(Handle<AbstractCode> code,
                           Handle<SharedFunctionInfo> shared) override {}
  void SnapshotPositionEvent(HeapObject obj, int pos);
  void CodeMovingGCEvent() override;

 private:
  void LogRecordedBuffer(Handle<AbstractCode> code,
                         MaybeHandle<SharedFunctionInfo> maybe_shared,
                         const char* name, int length) override;
#if V8_ENABLE_WEBASSEMBLY
  void LogRecordedBuffer(const wasm::WasmCode* code, const char* name,
                         int length) override;
#endif  // V8_ENABLE_WEBASSEMBLY

  // Low-level profiling event structures.
  struct CodeCreateStruct {
    static const char kTag = 'C';

    int32_t name_size;
    Address code_address;
    int32_t code_size;
  };

  struct CodeMoveStruct {
    static const char kTag = 'M';

    Address from_address;
    Address to_address;
  };

  static const char kCodeMovingGCTag = 'G';

  // Extension added to V8 log file name to get the low-level log name.
  static const char kLogExt[];

  void LogCodeInfo();
  void LogWriteBytes(const char* bytes, int size);

  template <typename T>
  void LogWriteStruct(const T& s) {
    char tag = T::kTag;
    LogWriteBytes(reinterpret_cast<const char*>(&tag), sizeof(tag));
    LogWriteBytes(reinterpret_cast<const char*>(&s), sizeof(s));
  }

  FILE* ll_output_handle_;
};

const char LowLevelLogger::kLogExt[] = ".ll";

LowLevelLogger::LowLevelLogger(Isolate* isolate, const char* name)
    : CodeEventLogger(isolate), ll_output_handle_(nullptr) {
  // Open the low-level log file.
  size_t len = strlen(name);
  base::ScopedVector<char> ll_name(static_cast<int>(len + sizeof(kLogExt)));
  MemCopy(ll_name.begin(), name, len);
  MemCopy(ll_name.begin() + len, kLogExt, sizeof(kLogExt));
  ll_output_handle_ =
      base::OS::FOpen(ll_name.begin(), base::OS::LogFileOpenMode);
  setvbuf(ll_output_handle_, nullptr, _IOLBF, 0);

  LogCodeInfo();
}

LowLevelLogger::~LowLevelLogger() {
  base::Fclose(ll_output_handle_);
  ll_output_handle_ = nullptr;
}

void LowLevelLogger::LogCodeInfo() {
#if V8_TARGET_ARCH_IA32
  const char arch[] = "ia32";
#elif V8_TARGET_ARCH_X64 && V8_TARGET_ARCH_64_BIT
  const char arch[] = "x64";
#elif V8_TARGET_ARCH_ARM
  const char arch[] = "arm";
#elif V8_TARGET_ARCH_PPC
  const char arch[] = "ppc";
#elif V8_TARGET_ARCH_PPC64
  const char arch[] = "ppc64";
#elif V8_TARGET_ARCH_MIPS
  const char arch[] = "mips";
#elif V8_TARGET_ARCH_LOONG64
  const char arch[] = "loong64";
#elif V8_TARGET_ARCH_ARM64
  const char arch[] = "arm64";
#elif V8_TARGET_ARCH_S390
  const char arch[] = "s390";
#elif V8_TARGET_ARCH_RISCV64
  const char arch[] = "riscv64";
#elif V8_TARGET_ARCH_RISCV32
  const char arch[] = "riscv32";
#else
  const char arch[] = "unknown";
#endif
  LogWriteBytes(arch, sizeof(arch));
}

void LowLevelLogger::LogRecordedBuffer(Handle<AbstractCode> code,
                                       MaybeHandle<SharedFunctionInfo>,
                                       const char* name, int length) {
  PtrComprCageBase cage_base(isolate_);
  CodeCreateStruct event;
  event.name_size = length;
  event.code_address = code->InstructionStart(cage_base);
  event.code_size = code->InstructionSize(cage_base);
  LogWriteStruct(event);
  LogWriteBytes(name, length);
  LogWriteBytes(
      reinterpret_cast<const char*>(code->InstructionStart(cage_base)),
      code->InstructionSize(cage_base));
}

#if V8_ENABLE_WEBASSEMBLY
void LowLevelLogger::LogRecordedBuffer(const wasm::WasmCode* code,
                                       const char* name, int length) {
  CodeCreateStruct event;
  event.name_size = length;
  event.code_address = code->instruction_start();
  event.code_size = code->instructions().length();
  LogWriteStruct(event);
  LogWriteBytes(name, length);
  LogWriteBytes(reinterpret_cast<const char*>(code->instruction_start()),
                code->instructions().length());
}
#endif  // V8_ENABLE_WEBASSEMBLY

void LowLevelLogger::CodeMoveEvent(AbstractCode from, AbstractCode to) {
  PtrComprCageBase cage_base(isolate_);
  CodeMoveStruct event;
  event.from_address = from.InstructionStart(cage_base);
  event.to_address = to.InstructionStart(cage_base);
  LogWriteStruct(event);
}

void LowLevelLogger::LogWriteBytes(const char* bytes, int size) {
  size_t rv = fwrite(bytes, 1, size, ll_output_handle_);
  DCHECK(static_cast<size_t>(size) == rv);
  USE(rv);
}

void LowLevelLogger::CodeMovingGCEvent() {
  const char tag = kCodeMovingGCTag;

  LogWriteBytes(&tag, sizeof(tag));
}

class JitLogger : public CodeEventLogger {
 public:
  JitLogger(Isolate* isolate, JitCodeEventHandler code_event_handler);

  void CodeMoveEvent(AbstractCode from, AbstractCode to) override;
  void CodeDisableOptEvent(Handle<AbstractCode> code,
                           Handle<SharedFunctionInfo> shared) override {}
  void AddCodeLinePosInfoEvent(void* jit_handler_data, int pc_offset,
                               int position,
                               JitCodeEvent::PositionType position_type,
                               JitCodeEvent::CodeType code_type);

  void* StartCodePosInfoEvent(JitCodeEvent::CodeType code_type);
  void EndCodePosInfoEvent(Address start_address, void* jit_handler_data,
                           JitCodeEvent::CodeType code_type);

 private:
  void LogRecordedBuffer(Handle<AbstractCode> code,
                         MaybeHandle<SharedFunctionInfo> maybe_shared,
                         const char* name, int length) override;
#if V8_ENABLE_WEBASSEMBLY
  void LogRecordedBuffer(const wasm::WasmCode* code, const char* name,
                         int length) override;
#endif  // V8_ENABLE_WEBASSEMBLY

  JitCodeEventHandler code_event_handler_;
  base::Mutex logger_mutex_;
};

JitLogger::JitLogger(Isolate* isolate, JitCodeEventHandler code_event_handler)
    : CodeEventLogger(isolate), code_event_handler_(code_event_handler) {
  DCHECK_NOT_NULL(code_event_handler);
}

void JitLogger::LogRecordedBuffer(Handle<AbstractCode> code,
                                  MaybeHandle<SharedFunctionInfo> maybe_shared,
                                  const char* name, int length) {
  PtrComprCageBase cage_base(isolate_);
  JitCodeEvent event;
  event.type = JitCodeEvent::CODE_ADDED;
  event.code_start = reinterpret_cast<void*>(code->InstructionStart(cage_base));
  event.code_type = code->IsCode(cage_base) ? JitCodeEvent::JIT_CODE
                                            : JitCodeEvent::BYTE_CODE;
  event.code_len = code->InstructionSize(cage_base);
  Handle<SharedFunctionInfo> shared;
  if (maybe_shared.ToHandle(&shared) &&
      shared->script(cage_base).IsScript(cage_base)) {
    event.script = ToApiHandle<v8::UnboundScript>(shared);
  } else {
    event.script = Local<v8::UnboundScript>();
  }
  event.name.str = name;
  event.name.len = length;
  event.isolate = reinterpret_cast<v8::Isolate*>(isolate_);
  code_event_handler_(&event);
}

#if V8_ENABLE_WEBASSEMBLY
void JitLogger::LogRecordedBuffer(const wasm::WasmCode* code, const char* name,
                                  int length) {
  JitCodeEvent event;
  event.type = JitCodeEvent::CODE_ADDED;
  event.code_type = JitCodeEvent::WASM_CODE;
  event.code_start = code->instructions().begin();
  event.code_len = code->instructions().length();
  event.name.str = name;
  event.name.len = length;
  event.isolate = reinterpret_cast<v8::Isolate*>(isolate_);

  wasm::WasmModuleSourceMap* source_map =
      code->native_module()->GetWasmSourceMap();
  wasm::WireBytesRef code_ref =
      code->native_module()->module()->functions[code->index()].code;
  uint32_t code_offset = code_ref.offset();
  uint32_t code_end_offset = code_ref.end_offset();

  std::vector<v8::JitCodeEvent::line_info_t> mapping_info;
  std::string filename;
  std::unique_ptr<JitCodeEvent::wasm_source_info_t> wasm_source_info;

  if (source_map && source_map->IsValid() &&
      source_map->HasSource(code_offset, code_end_offset)) {
    size_t last_line_number = 0;

    for (SourcePositionTableIterator iterator(code->source_positions());
         !iterator.done(); iterator.Advance()) {
      uint32_t offset = iterator.source_position().ScriptOffset() + code_offset;
      if (!source_map->HasValidEntry(code_offset, offset)) continue;
      if (filename.empty()) {
        filename = source_map->GetFilename(offset);
      }
      mapping_info.push_back({static_cast<size_t>(iterator.code_offset()),
                              last_line_number, JitCodeEvent::POSITION});
      last_line_number = source_map->GetSourceLine(offset) + 1;
    }

    wasm_source_info = std::make_unique<JitCodeEvent::wasm_source_info_t>();
    wasm_source_info->filename = filename.c_str();
    wasm_source_info->filename_size = filename.size();
    wasm_source_info->line_number_table_size = mapping_info.size();
    wasm_source_info->line_number_table = mapping_info.data();

    event.wasm_source_info = wasm_source_info.get();
  }
  code_event_handler_(&event);
}
#endif  // V8_ENABLE_WEBASSEMBLY

void JitLogger::CodeMoveEvent(AbstractCode from, AbstractCode to) {
  base::MutexGuard guard(&logger_mutex_);

  PtrComprCageBase cage_base(isolate_);
  JitCodeEvent event;
  event.type = JitCodeEvent::CODE_MOVED;
  event.code_type =
      from.IsCode(cage_base) ? JitCodeEvent::JIT_CODE : JitCodeEvent::BYTE_CODE;
  event.code_start = reinterpret_cast<void*>(from.InstructionStart(cage_base));
  event.code_len = from.InstructionSize(cage_base);
  event.new_code_start =
      reinterpret_cast<void*>(to.InstructionStart(cage_base));
  event.isolate = reinterpret_cast<v8::Isolate*>(isolate_);

  code_event_handler_(&event);
}

void JitLogger::AddCodeLinePosInfoEvent(
    void* jit_handler_data, int pc_offset, int position,
    JitCodeEvent::PositionType position_type,
    JitCodeEvent::CodeType code_type) {
  JitCodeEvent event;
  event.type = JitCodeEvent::CODE_ADD_LINE_POS_INFO;
  event.code_type = code_type;
  event.user_data = jit_handler_data;
  event.line_info.offset = pc_offset;
  event.line_info.pos = position;
  event.line_info.position_type = position_type;
  event.isolate = reinterpret_cast<v8::Isolate*>(isolate_);

  code_event_handler_(&event);
}

void* JitLogger::StartCodePosInfoEvent(JitCodeEvent::CodeType code_type) {
  JitCodeEvent event;
  event.type = JitCodeEvent::CODE_START_LINE_INFO_RECORDING;
  event.code_type = code_type;
  event.isolate = reinterpret_cast<v8::Isolate*>(isolate_);

  code_event_handler_(&event);
  return event.user_data;
}

void JitLogger::EndCodePosInfoEvent(Address start_address,
                                    void* jit_handler_data,
                                    JitCodeEvent::CodeType code_type) {
  JitCodeEvent event;
  event.type = JitCodeEvent::CODE_END_LINE_INFO_RECORDING;
  event.code_type = code_type;
  event.code_start = reinterpret_cast<void*>(start_address);
  event.user_data = jit_handler_data;
  event.isolate = reinterpret_cast<v8::Isolate*>(isolate_);

  code_event_handler_(&event);
}

// TODO(lpy): Keeping sampling thread inside V8 is a workaround currently,
// the reason is to reduce code duplication during migration to sampler library,
// sampling thread, as well as the sampler, will be moved to D8 eventually.
class SamplingThread : public base::Thread {
 public:
  static const int kSamplingThreadStackSize = 64 * KB;

  SamplingThread(sampler::Sampler* sampler, int interval_microseconds)
      : base::Thread(
            base::Thread::Options("SamplingThread", kSamplingThreadStackSize)),
        sampler_(sampler),
        interval_microseconds_(interval_microseconds) {}
  void Run() override {
    while (sampler_->IsActive()) {
      sampler_->DoSample();
      base::OS::Sleep(
          base::TimeDelta::FromMicroseconds(interval_microseconds_));
    }
  }

 private:
  sampler::Sampler* sampler_;
  const int interval_microseconds_;
};

// The Profiler samples pc and sp values for the main thread.
// Each sample is appended to a circular buffer.
// An independent thread removes data and writes it to the log.
// This design minimizes the time spent in the sampler.
//
class Profiler : public base::Thread {
 public:
  explicit Profiler(Isolate* isolate);
  void Engage();
  void Disengage();

  // Inserts collected profiling data into buffer.
  void Insert(TickSample* sample) {
    if (Succ(head_) == static_cast<int>(base::Acquire_Load(&tail_))) {
      overflow_ = true;
    } else {
      buffer_[head_] = *sample;
      head_ = Succ(head_);
      buffer_semaphore_.Signal();  // Tell we have an element.
    }
  }

  void Run() override;

 private:
  // Waits for a signal and removes profiling data.
  bool Remove(TickSample* sample) {
    buffer_semaphore_.Wait();  // Wait for an element.
    *sample = buffer_[base::Relaxed_Load(&tail_)];
    bool result = overflow_;
    base::Release_Store(
        &tail_, static_cast<base::Atomic32>(Succ(base::Relaxed_Load(&tail_))));
    overflow_ = false;
    return result;
  }

  // Returns the next index in the cyclic buffer.
  int Succ(int index) { return (index + 1) % kBufferSize; }

  Isolate* isolate_;
  // Cyclic buffer for communicating profiling samples
  // between the signal handler and the worker thread.
  static const int kBufferSize = 128;
  TickSample buffer_[kBufferSize];  // Buffer storage.
  int head_;                        // Index to the buffer head.
  base::Atomic32 tail_;             // Index to the buffer tail.
  bool overflow_;  // Tell whether a buffer overflow has occurred.
  // Semaphore used for buffer synchronization.
  base::Semaphore buffer_semaphore_;

  // Tells whether worker thread should continue running.
  base::Atomic32 running_;
};

//
// Ticker used to provide ticks to the profiler and the sliding state
// window.
//
class Ticker : public sampler::Sampler {
 public:
  Ticker(Isolate* isolate, int interval_microseconds)
      : sampler::Sampler(reinterpret_cast<v8::Isolate*>(isolate)),
        sampling_thread_(
            std::make_unique<SamplingThread>(this, interval_microseconds)),
        perThreadData_(isolate->FindPerThreadDataForThisThread()) {}

  ~Ticker() override {
    if (IsActive()) Stop();
  }

  void SetProfiler(Profiler* profiler) {
    DCHECK_NULL(profiler_);
    profiler_ = profiler;
    if (!IsActive()) Start();
    sampling_thread_->StartSynchronously();
  }

  void ClearProfiler() {
    profiler_ = nullptr;
    if (IsActive()) Stop();
    sampling_thread_->Join();
  }

  void SampleStack(const v8::RegisterState& state) override {
    if (!profiler_) return;
    Isolate* isolate = reinterpret_cast<Isolate*>(this->isolate());
    if (isolate->was_locker_ever_used() &&
        (!isolate->thread_manager()->IsLockedByThread(
             perThreadData_->thread_id()) ||
         perThreadData_->thread_state() != nullptr))
      return;
    TickSample sample;
    sample.Init(isolate, state, TickSample::kIncludeCEntryFrame, true);
    profiler_->Insert(&sample);
  }

 private:
  Profiler* profiler_ = nullptr;
  std::unique_ptr<SamplingThread> sampling_thread_;
  Isolate::PerIsolateThreadData* perThreadData_;
};

//
// Profiler implementation when invoking with --prof.
//
Profiler::Profiler(Isolate* isolate)
    : base::Thread(Options("v8:Profiler")),
      isolate_(isolate),
      head_(0),
      overflow_(false),
      buffer_semaphore_(0) {
  base::Relaxed_Store(&tail_, 0);
  base::Relaxed_Store(&running_, 0);
}

void Profiler::Engage() {
  std::vector<base::OS::SharedLibraryAddress> addresses =
      base::OS::GetSharedLibraryAddresses();
  for (const auto& address : addresses) {
    LOG(isolate_, SharedLibraryEvent(address.library_path, address.start,
                                     address.end, address.aslr_slide));
  }
  LOG(isolate_, SharedLibraryEnd());

  // Start thread processing the profiler buffer.
  base::Relaxed_Store(&running_, 1);
  CHECK(Start());

  // Register to get ticks.
  V8FileLogger* logger = isolate_->v8_file_logger();
  logger->ticker_->SetProfiler(this);

  LOG(isolate_, ProfilerBeginEvent());
}

void Profiler::Disengage() {
  // Stop receiving ticks.
  isolate_->v8_file_logger()->ticker_->ClearProfiler();

  // Terminate the worker thread by setting running_ to false,
  // inserting a fake element in the queue and then wait for
  // the thread to terminate.
  base::Relaxed_Store(&running_, 0);
  TickSample sample;
  Insert(&sample);
  Join();

  LOG(isolate_, UncheckedStringEvent("profiler", "end"));
}

void Profiler::Run() {
  TickSample sample;
  bool overflow = Remove(&sample);
  while (base::Relaxed_Load(&running_)) {
    LOG(isolate_, TickEvent(&sample, overflow));
    overflow = Remove(&sample);
  }
}

//
// V8FileLogger class implementation.
//
#define MSG_BUILDER()                                \
  std::unique_ptr<LogFile::MessageBuilder> msg_ptr = \
      log_->NewMessageBuilder();                     \
  if (!msg_ptr) return;                              \
  LogFile::MessageBuilder& msg = *msg_ptr.get();

V8FileLogger::V8FileLogger(Isolate* isolate)
    : isolate_(isolate),
      is_logging_(false),
      is_initialized_(false),
      existing_code_logger_(isolate) {}

V8FileLogger::~V8FileLogger() = default;

const LogSeparator V8FileLogger::kNext = LogSeparator::kSeparator;

int64_t V8FileLogger::Time() {
  if (FLAG_verify_predictable) {
    return isolate_->heap()->MonotonicallyIncreasingTimeInMs() * 1000;
  }
  return timer_.Elapsed().InMicroseconds();
}

void V8FileLogger::AddLogEventListener(LogEventListener* listener) {
  bool result = isolate_->logger()->AddListener(listener);
  CHECK(result);
}

void V8FileLogger::RemoveLogEventListener(LogEventListener* listener) {
  isolate_->logger()->RemoveListener(listener);
}

void V8FileLogger::ProfilerBeginEvent() {
  MSG_BUILDER();
  msg << "profiler" << kNext << "begin" << kNext << FLAG_prof_sampling_interval;
  msg.WriteToLogFile();
}

void V8FileLogger::StringEvent(const char* name, const char* value) {
  if (FLAG_log) UncheckedStringEvent(name, value);
}

void V8FileLogger::UncheckedStringEvent(const char* name, const char* value) {
  MSG_BUILDER();
  msg << name << kNext << value;
  msg.WriteToLogFile();
}

void V8FileLogger::IntPtrTEvent(const char* name, intptr_t value) {
  if (!FLAG_log) return;
  MSG_BUILDER();
  msg << name << kNext;
  msg.AppendFormatString("%" V8PRIdPTR, value);
  msg.WriteToLogFile();
}

void V8FileLogger::SharedLibraryEvent(const std::string& library_path,
                                      uintptr_t start, uintptr_t end,
                                      intptr_t aslr_slide) {
  if (!FLAG_prof_cpp) return;
  MSG_BUILDER();
  msg << "shared-library" << kNext << library_path.c_str() << kNext
      << reinterpret_cast<void*>(start) << kNext << reinterpret_cast<void*>(end)
      << kNext << aslr_slide;
  msg.WriteToLogFile();
}

void V8FileLogger::SharedLibraryEnd() {
  if (!FLAG_prof_cpp) return;
  MSG_BUILDER();
  msg << "shared-library-end";
  msg.WriteToLogFile();
}

void V8FileLogger::CurrentTimeEvent() {
  DCHECK(FLAG_log_internal_timer_events);
  MSG_BUILDER();
  msg << "current-time" << kNext << Time();
  msg.WriteToLogFile();
}

void V8FileLogger::TimerEvent(v8::LogEventStatus se, const char* name) {
  MSG_BUILDER();
  switch (se) {
    case kStart:
      msg << "timer-event-start";
      break;
    case kEnd:
      msg << "timer-event-end";
      break;
    case kStamp:
      msg << "timer-event";
  }
  msg << kNext << name << kNext << Time();
  msg.WriteToLogFile();
}

bool V8FileLogger::is_logging() {
  // Disable logging while the CPU profiler is running.
  if (isolate_->is_profiling()) return false;
  return is_logging_.load(std::memory_order_relaxed);
}

// Instantiate template methods.
#define V(TimerName, expose)                                           \
  template void TimerEventScope<TimerEvent##TimerName>::LogTimerEvent( \
      v8::LogEventStatus se);
TIMER_EVENTS_LIST(V)
#undef V

void V8FileLogger::NewEvent(const char* name, void* object, size_t size) {
  if (!FLAG_log) return;
  MSG_BUILDER();
  msg << "new" << kNext << name << kNext << object << kNext
      << static_cast<unsigned int>(size);
  msg.WriteToLogFile();
}

void V8FileLogger::DeleteEvent(const char* name, void* object) {
  if (!FLAG_log) return;
  MSG_BUILDER();
  msg << "delete" << kNext << name << kNext << object;
  msg.WriteToLogFile();
}

namespace {

void AppendCodeCreateHeader(LogFile::MessageBuilder& msg,
                            LogEventListener::CodeTag tag, CodeKind kind,
                            uint8_t* address, int size, uint64_t time) {
  msg << LogEventListener::Event::kCodeCreation << V8FileLogger::kNext << tag
      << V8FileLogger::kNext << static_cast<int>(kind) << V8FileLogger::kNext
      << time << V8FileLogger::kNext << reinterpret_cast<void*>(address)
      << V8FileLogger::kNext << size << V8FileLogger::kNext;
}

void AppendCodeCreateHeader(Isolate* isolate, LogFile::MessageBuilder& msg,
                            LogEventListener::CodeTag tag, AbstractCode code,
                            uint64_t time) {
  PtrComprCageBase cage_base(isolate);
  AppendCodeCreateHeader(
      msg, tag, code.kind(cage_base),
      reinterpret_cast<uint8_t*>(code.InstructionStart(cage_base)),
      code.InstructionSize(cage_base), time);
}

}  // namespace
// We log source code information in the form:
//
// code-source-info <addr>,<script>,<start>,<end>,<pos>,<inline-pos>,<fns>
//
// where
//   <addr> is code object address
//   <script> is script id
//   <start> is the starting position inside the script
//   <end> is the end position inside the script
//   <pos> is source position table encoded in the string,
//      it is a sequence of C<code-offset>O<script-offset>[I<inlining-id>]
//      where
//        <code-offset> is the offset within the code object
//        <script-offset> is the position within the script
//        <inlining-id> is the offset in the <inlining> table
//   <inlining> table is a sequence of strings of the form
//      F<function-id>O<script-offset>[I<inlining-id>]
//      where
//         <function-id> is an index into the <fns> function table
//   <fns> is the function table encoded as a sequence of strings
//      S<shared-function-info-address>

void V8FileLogger::LogSourceCodeInformation(Handle<AbstractCode> code,
                                            Handle<SharedFunctionInfo> shared) {
  PtrComprCageBase cage_base(isolate_);
  Object script_object = shared->script(cage_base);
  if (!script_object.IsScript(cage_base)) return;
  Script script = Script::cast(script_object);
  EnsureLogScriptSource(script);

  MSG_BUILDER();
  msg << "code-source-info" << V8FileLogger::kNext
      << reinterpret_cast<void*>(code->InstructionStart(cage_base))
      << V8FileLogger::kNext << script.id() << V8FileLogger::kNext
      << shared->StartPosition() << V8FileLogger::kNext << shared->EndPosition()
      << V8FileLogger::kNext;
  // TODO(v8:11429): Clean-up baseline-replated code in source position
  // iteration.
  bool hasInlined = false;
  if (code->kind(cage_base) != CodeKind::BASELINE) {
    SourcePositionTableIterator iterator(
        code->SourcePositionTable(cage_base, *shared));
    for (; !iterator.done(); iterator.Advance()) {
      SourcePosition pos = iterator.source_position();
      msg << "C" << iterator.code_offset() << "O" << pos.ScriptOffset();
      if (pos.isInlined()) {
        msg << "I" << pos.InliningId();
        hasInlined = true;
      }
    }
  }
  msg << V8FileLogger::kNext;
  int maxInlinedId = -1;
  if (hasInlined) {
    PodArray<InliningPosition> inlining_positions =
        DeoptimizationData::cast(
            Handle<Code>::cast(code)->deoptimization_data())
            .InliningPositions();
    for (int i = 0; i < inlining_positions.length(); i++) {
      InliningPosition inlining_pos = inlining_positions.get(i);
      msg << "F";
      if (inlining_pos.inlined_function_id != -1) {
        msg << inlining_pos.inlined_function_id;
        if (inlining_pos.inlined_function_id > maxInlinedId) {
          maxInlinedId = inlining_pos.inlined_function_id;
        }
      }
      SourcePosition pos = inlining_pos.position;
      msg << "O" << pos.ScriptOffset();
      if (pos.isInlined()) {
        msg << "I" << pos.InliningId();
      }
    }
  }
  msg << V8FileLogger::kNext;
  if (hasInlined) {
    DeoptimizationData deopt_data = DeoptimizationData::cast(
        Handle<Code>::cast(code)->deoptimization_data());
    msg << std::hex;
    for (int i = 0; i <= maxInlinedId; i++) {
      msg << "S"
          << reinterpret_cast<void*>(
                 deopt_data.GetInlinedFunction(i).address());
    }
    msg << std::dec;
  }
  msg.WriteToLogFile();
}

void V8FileLogger::LogCodeDisassemble(Handle<AbstractCode> code) {
  if (!FLAG_log_code_disassemble) return;
  PtrComprCageBase cage_base(isolate_);
  MSG_BUILDER();
  msg << "code-disassemble" << V8FileLogger::kNext
      << reinterpret_cast<void*>(code->InstructionStart(cage_base))
      << V8FileLogger::kNext << CodeKindToString(code->kind(cage_base))
      << V8FileLogger::kNext;
  {
    std::ostringstream stream;
    if (code->IsCode(cage_base)) {
#ifdef ENABLE_DISASSEMBLER
      Code::cast(*code).Disassemble(nullptr, stream, isolate_);
#endif
    } else if (V8_REMOVE_BUILTINS_CODE_OBJECTS &&
               code->IsCodeDataContainer(cage_base)) {
#ifdef ENABLE_DISASSEMBLER
      CodeT::cast(*code).Disassemble(nullptr, stream, isolate_);
#endif
    } else {
      BytecodeArray::cast(*code).Disassemble(stream);
    }
    std::string string = stream.str();
    msg.AppendString(string.c_str(), string.length());
  }
  msg.WriteToLogFile();
}

// Builtins and Bytecode handlers
void V8FileLogger::CodeCreateEvent(CodeTag tag, Handle<AbstractCode> code,
                                   const char* name) {
  if (!is_listening_to_code_events()) return;
  if (!FLAG_log_code) return;
  {
    MSG_BUILDER();
    AppendCodeCreateHeader(isolate_, msg, tag, *code, Time());
    msg << name;
    msg.WriteToLogFile();
  }
  LogCodeDisassemble(code);
}

void V8FileLogger::CodeCreateEvent(CodeTag tag, Handle<AbstractCode> code,
                                   Handle<Name> name) {
  if (!is_listening_to_code_events()) return;
  if (!FLAG_log_code) return;
  {
    MSG_BUILDER();
    AppendCodeCreateHeader(isolate_, msg, tag, *code, Time());
    msg << *name;
    msg.WriteToLogFile();
  }
  LogCodeDisassemble(code);
}

// Scripts
void V8FileLogger::CodeCreateEvent(CodeTag tag, Handle<AbstractCode> code,
                                   Handle<SharedFunctionInfo> shared,
                                   Handle<Name> script_name) {
  if (!is_listening_to_code_events()) return;
  if (!FLAG_log_code) return;
  if (*code ==
      AbstractCode::cast(isolate_->builtins()->code(Builtin::kCompileLazy))) {
    return;
  }
  {
    MSG_BUILDER();
    AppendCodeCreateHeader(isolate_, msg, tag, *code, Time());
    msg << *script_name << kNext << reinterpret_cast<void*>(shared->address())
        << kNext << ComputeMarker(*shared, *code);
    msg.WriteToLogFile();
  }
  LogSourceCodeInformation(code, shared);
  LogCodeDisassemble(code);
}

void V8FileLogger::FeedbackVectorEvent(FeedbackVector vector,
                                       AbstractCode code) {
  DisallowGarbageCollection no_gc;
  if (!FLAG_log_code) return;
  PtrComprCageBase cage_base(isolate_);
  MSG_BUILDER();
  msg << "feedback-vector" << kNext << Time();
  msg << kNext << reinterpret_cast<void*>(vector.address()) << kNext
      << vector.length();
  msg << kNext << reinterpret_cast<void*>(code.InstructionStart(cage_base));
  msg << kNext << vector.tiering_state();
  msg << kNext << vector.maybe_has_maglev_code();
  msg << kNext << vector.maybe_has_turbofan_code();
  msg << kNext << vector.invocation_count();
  msg << kNext << vector.profiler_ticks() << kNext;

#ifdef OBJECT_PRINT
  std::ostringstream buffer;
  vector.FeedbackVectorPrint(buffer);
  std::string contents = buffer.str();
  msg.AppendString(contents.c_str(), contents.length());
#else
  msg << "object-printing-disabled";
#endif
  msg.WriteToLogFile();
}

// Functions
// Although, it is possible to extract source and line from
// the SharedFunctionInfo object, we left it to caller
// to leave logging functions free from heap allocations.
void V8FileLogger::CodeCreateEvent(CodeTag tag, Handle<AbstractCode> code,
                                   Handle<SharedFunctionInfo> shared,
                                   Handle<Name> script_name, int line,
                                   int column) {
  if (!is_listening_to_code_events()) return;
  if (!FLAG_log_code) return;
  {
    MSG_BUILDER();
    AppendCodeCreateHeader(isolate_, msg, tag, *code, Time());
    msg << shared->DebugNameCStr().get() << " " << *script_name << ":" << line
        << ":" << column << kNext << reinterpret_cast<void*>(shared->address())
        << kNext << ComputeMarker(*shared, *code);

    msg.WriteToLogFile();
  }
  LogSourceCodeInformation(code, shared);
  LogCodeDisassemble(code);
}

#if V8_ENABLE_WEBASSEMBLY
void V8FileLogger::CodeCreateEvent(CodeTag tag, const wasm::WasmCode* code,
                                   wasm::WasmName name,
                                   const char* /*source_url*/,
                                   int /*code_offset*/, int /*script_id*/) {
  if (!is_listening_to_code_events()) return;
  if (!FLAG_log_code) return;
  MSG_BUILDER();
  AppendCodeCreateHeader(msg, tag, CodeKind::WASM_FUNCTION,
                         code->instructions().begin(),
                         code->instructions().length(), Time());
  DCHECK(!name.empty());
  msg.AppendString(name);

  // We have to add two extra fields that allow the tick processor to group
  // events for the same wasm function, even if it gets compiled again. For
  // normal JS functions, we use the shared function info. For wasm, the pointer
  // to the native module + function index works well enough.
  // TODO(herhut) Clean up the tick processor code instead.
  void* tag_ptr =
      reinterpret_cast<byte*>(code->native_module()) + code->index();
  msg << kNext << tag_ptr << kNext << ComputeMarker(code);
  msg.WriteToLogFile();
}
#endif  // V8_ENABLE_WEBASSEMBLY

void V8FileLogger::CallbackEventInternal(const char* prefix, Handle<Name> name,
                                         Address entry_point) {
  if (!FLAG_log_code) return;
  MSG_BUILDER();
  msg << Event::kCodeCreation << kNext << CodeTag::kCallback << kNext << -2
      << kNext << Time() << kNext << reinterpret_cast<void*>(entry_point)
      << kNext << 1 << kNext << prefix << *name;
  msg.WriteToLogFile();
}

void V8FileLogger::CallbackEvent(Handle<Name> name, Address entry_point) {
  CallbackEventInternal("", name, entry_point);
}

void V8FileLogger::GetterCallbackEvent(Handle<Name> name, Address entry_point) {
  CallbackEventInternal("get ", name, entry_point);
}

void V8FileLogger::SetterCallbackEvent(Handle<Name> name, Address entry_point) {
  CallbackEventInternal("set ", name, entry_point);
}

void V8FileLogger::RegExpCodeCreateEvent(Handle<AbstractCode> code,
                                         Handle<String> source) {
  if (!is_listening_to_code_events()) return;
  if (!FLAG_log_code) return;
  MSG_BUILDER();
  AppendCodeCreateHeader(isolate_, msg, LogEventListener::CodeTag::kRegExp,
                         *code, Time());
  msg << *source;
  msg.WriteToLogFile();
}

void V8FileLogger::CodeMoveEvent(AbstractCode from, AbstractCode to) {
  if (!is_listening_to_code_events()) return;
  PtrComprCageBase cage_base(isolate_);
  MoveEventInternal(Event::kCodeMove, from.InstructionStart(cage_base),
                    to.InstructionStart(cage_base));
}

void V8FileLogger::SharedFunctionInfoMoveEvent(Address from, Address to) {
  if (!is_listening_to_code_events()) return;
  MoveEventInternal(Event::kSharedFuncMove, from, to);
}

void V8FileLogger::CodeMovingGCEvent() {
  if (!is_listening_to_code_events()) return;
  if (!FLAG_ll_prof) return;
  base::OS::SignalCodeMovingGC();
}

void V8FileLogger::CodeDisableOptEvent(Handle<AbstractCode> code,
                                       Handle<SharedFunctionInfo> shared) {
  if (!is_listening_to_code_events()) return;
  if (!FLAG_log_code) return;
  MSG_BUILDER();
  msg << Event::kCodeDisableOpt << kNext << shared->DebugNameCStr().get()
      << kNext << GetBailoutReason(shared->disabled_optimization_reason());
  msg.WriteToLogFile();
}

void V8FileLogger::ProcessDeoptEvent(Handle<Code> code, SourcePosition position,
                                     const char* kind, const char* reason) {
  MSG_BUILDER();
  msg << Event::kCodeDeopt << kNext << Time() << kNext << code->CodeSize()
      << kNext << reinterpret_cast<void*>(code->InstructionStart());

  std::ostringstream deopt_location;
  int inlining_id = -1;
  int script_offset = -1;
  if (position.IsKnown()) {
    position.Print(deopt_location, *code);
    inlining_id = position.InliningId();
    script_offset = position.ScriptOffset();
  } else {
    deopt_location << "<unknown>";
  }
  msg << kNext << inlining_id << kNext << script_offset << kNext;
  msg << kind << kNext;
  msg << deopt_location.str().c_str() << kNext << reason;
  msg.WriteToLogFile();
}

void V8FileLogger::CodeDeoptEvent(Handle<Code> code, DeoptimizeKind kind,
                                  Address pc, int fp_to_sp_delta) {
  if (!is_logging() || !FLAG_log_deopt) return;
  Deoptimizer::DeoptInfo info = Deoptimizer::GetDeoptInfo(*code, pc);
  ProcessDeoptEvent(code, info.position, Deoptimizer::MessageFor(kind),
                    DeoptimizeReasonToString(info.deopt_reason));
}

void V8FileLogger::CodeDependencyChangeEvent(Handle<Code> code,
                                             Handle<SharedFunctionInfo> sfi,
                                             const char* reason) {
  if (!is_logging() || !FLAG_log_deopt) return;
  SourcePosition position(sfi->StartPosition(), -1);
  ProcessDeoptEvent(code, position, "dependency-change", reason);
}

namespace {

void CodeLinePosEvent(JitLogger& jit_logger, Address code_start,
                      SourcePositionTableIterator& iter,
                      JitCodeEvent::CodeType code_type) {
  void* jit_handler_data = jit_logger.StartCodePosInfoEvent(code_type);
  for (; !iter.done(); iter.Advance()) {
    if (iter.is_statement()) {
      jit_logger.AddCodeLinePosInfoEvent(jit_handler_data, iter.code_offset(),
                                         iter.source_position().ScriptOffset(),
                                         JitCodeEvent::STATEMENT_POSITION,
                                         code_type);
    }
    jit_logger.AddCodeLinePosInfoEvent(jit_handler_data, iter.code_offset(),
                                       iter.source_position().ScriptOffset(),
                                       JitCodeEvent::POSITION, code_type);
  }
  jit_logger.EndCodePosInfoEvent(code_start, jit_handler_data, code_type);
}

}  // namespace

void V8FileLogger::CodeLinePosInfoRecordEvent(
    Address code_start, ByteArray source_position_table,
    JitCodeEvent::CodeType code_type) {
  if (!jit_logger_) return;
  SourcePositionTableIterator iter(source_position_table);
  CodeLinePosEvent(*jit_logger_, code_start, iter, code_type);
}

#if V8_ENABLE_WEBASSEMBLY
void V8FileLogger::WasmCodeLinePosInfoRecordEvent(
    Address code_start, base::Vector<const byte> source_position_table) {
  if (!jit_logger_) return;
  SourcePositionTableIterator iter(source_position_table);
  CodeLinePosEvent(*jit_logger_, code_start, iter, JitCodeEvent::WASM_CODE);
}
#endif  // V8_ENABLE_WEBASSEMBLY

void V8FileLogger::CodeNameEvent(Address addr, int pos, const char* code_name) {
  if (code_name == nullptr) return;  // Not a code object.
  if (!is_listening_to_code_events()) return;
  MSG_BUILDER();
  msg << Event::kSnapshotCodeName << kNext << pos << kNext << code_name;
  msg.WriteToLogFile();
}

void V8FileLogger::MoveEventInternal(Event event, Address from, Address to) {
  if (!FLAG_log_code) return;
  MSG_BUILDER();
  msg << event << kNext << reinterpret_cast<void*>(from) << kNext
      << reinterpret_cast<void*>(to);
  msg.WriteToLogFile();
}

namespace {
void AppendFunctionMessage(LogFile::MessageBuilder& msg, const char* reason,
                           int script_id, double time_delta, int start_position,
                           int end_position, uint64_t time) {
  msg << "function" << V8FileLogger::kNext << reason << V8FileLogger::kNext
      << script_id << V8FileLogger::kNext << start_position
      << V8FileLogger::kNext << end_position << V8FileLogger::kNext;
  if (V8_UNLIKELY(FLAG_predictable)) {
    msg << 0.1;
  } else {
    msg << time_delta;
  }
  msg << V8FileLogger::kNext << time << V8FileLogger::kNext;
}
}  // namespace

void V8FileLogger::FunctionEvent(const char* reason, int script_id,
                                 double time_delta, int start_position,
                                 int end_position, String function_name) {
  if (!FLAG_log_function_events) return;
  MSG_BUILDER();
  AppendFunctionMessage(msg, reason, script_id, time_delta, start_position,
                        end_position, Time());
  if (!function_name.is_null()) msg << function_name;
  msg.WriteToLogFile();
}

void V8FileLogger::FunctionEvent(const char* reason, int script_id,
                                 double time_delta, int start_position,
                                 int end_position, const char* function_name,
                                 size_t function_name_length,
                                 bool is_one_byte) {
  if (!FLAG_log_function_events) return;
  MSG_BUILDER();
  AppendFunctionMessage(msg, reason, script_id, time_delta, start_position,
                        end_position, Time());
  if (function_name_length > 0) {
    msg.AppendString(function_name, function_name_length, is_one_byte);
  }
  msg.WriteToLogFile();
}

void V8FileLogger::CompilationCacheEvent(const char* action,
                                         const char* cache_type,
                                         SharedFunctionInfo sfi) {
  if (!FLAG_log_function_events) return;
  MSG_BUILDER();
  int script_id = -1;
  if (sfi.script().IsScript()) {
    script_id = Script::cast(sfi.script()).id();
  }
  msg << "compilation-cache" << V8FileLogger::kNext << action
      << V8FileLogger::kNext << cache_type << V8FileLogger::kNext << script_id
      << V8FileLogger::kNext << sfi.StartPosition() << V8FileLogger::kNext
      << sfi.EndPosition() << V8FileLogger::kNext << Time();
  msg.WriteToLogFile();
}

void V8FileLogger::ScriptEvent(ScriptEventType type, int script_id) {
  if (!FLAG_log_function_events) return;
  MSG_BUILDER();
  msg << "script" << V8FileLogger::kNext;
  switch (type) {
    case ScriptEventType::kReserveId:
      msg << "reserve-id";
      break;
    case ScriptEventType::kCreate:
      msg << "create";
      break;
    case ScriptEventType::kDeserialize:
      msg << "deserialize";
      break;
    case ScriptEventType::kBackgroundCompile:
      msg << "background-compile";
      break;
    case ScriptEventType::kStreamingCompile:
      msg << "streaming-compile";
      break;
  }
  msg << V8FileLogger::kNext << script_id << V8FileLogger::kNext << Time();
  msg.WriteToLogFile();
}

void V8FileLogger::ScriptDetails(Script script) {
  if (!FLAG_log_function_events) return;
  {
    MSG_BUILDER();
    msg << "script-details" << V8FileLogger::kNext << script.id()
        << V8FileLogger::kNext;
    if (script.name().IsString()) {
      msg << String::cast(script.name());
    }
    msg << V8FileLogger::kNext << script.line_offset() << V8FileLogger::kNext
        << script.column_offset() << V8FileLogger::kNext;
    if (script.source_mapping_url().IsString()) {
      msg << String::cast(script.source_mapping_url());
    }
    msg.WriteToLogFile();
  }
  EnsureLogScriptSource(script);
}

bool V8FileLogger::EnsureLogScriptSource(Script script) {
  // Make sure the script is written to the log file.
  int script_id = script.id();
  if (logged_source_code_.find(script_id) != logged_source_code_.end()) {
    return true;
  }
  // This script has not been logged yet.
  logged_source_code_.insert(script_id);
  Object source_object = script.source();
  if (!source_object.IsString()) return false;

  std::unique_ptr<LogFile::MessageBuilder> msg_ptr = log_->NewMessageBuilder();
  if (!msg_ptr) return false;
  LogFile::MessageBuilder& msg = *msg_ptr.get();

  String source_code = String::cast(source_object);
  msg << "script-source" << kNext << script_id << kNext;

  // Log the script name.
  if (script.name().IsString()) {
    msg << String::cast(script.name()) << kNext;
  } else {
    msg << "<unknown>" << kNext;
  }

  // Log the source code.
  msg << source_code;
  msg.WriteToLogFile();
  return true;
}

void V8FileLogger::RuntimeCallTimerEvent() {
#ifdef V8_RUNTIME_CALL_STATS
  RuntimeCallStats* stats = isolate_->counters()->runtime_call_stats();
  RuntimeCallCounter* counter = stats->current_counter();
  if (counter == nullptr) return;
  MSG_BUILDER();
  msg << "active-runtime-timer" << kNext << counter->name();
  msg.WriteToLogFile();
#endif  // V8_RUNTIME_CALL_STATS
}

void V8FileLogger::TickEvent(TickSample* sample, bool overflow) {
  if (!FLAG_prof_cpp) return;
  if (V8_UNLIKELY(TracingFlags::runtime_stats.load(std::memory_order_relaxed) ==
                  v8::tracing::TracingCategoryObserver::ENABLED_BY_NATIVE)) {
    RuntimeCallTimerEvent();
  }
  MSG_BUILDER();
  msg << Event::kTick << kNext << reinterpret_cast<void*>(sample->pc) << kNext
      << Time();
  if (sample->has_external_callback) {
    msg << kNext << 1 << kNext
        << reinterpret_cast<void*>(sample->external_callback_entry);
  } else {
    msg << kNext << 0 << kNext << reinterpret_cast<void*>(sample->tos);
  }
  msg << kNext << static_cast<int>(sample->state);
  if (overflow) msg << kNext << "overflow";
  for (unsigned i = 0; i < sample->frames_count; ++i) {
    msg << kNext << reinterpret_cast<void*>(sample->stack[i]);
  }
  msg.WriteToLogFile();
}

void V8FileLogger::ICEvent(const char* type, bool keyed, Handle<Map> map,
                           Handle<Object> key, char old_state, char new_state,
                           const char* modifier, const char* slow_stub_reason) {
  if (!FLAG_log_ic) return;
  int line;
  int column;
  // GetAbstractPC must come before MSG_BUILDER(), as it can GC, which might
  // attempt to get the log lock again and result in a deadlock.
  Address pc = isolate_->GetAbstractPC(&line, &column);
  MSG_BUILDER();
  if (keyed) msg << "Keyed";
  msg << type << kNext << reinterpret_cast<void*>(pc) << kNext << Time()
      << kNext << line << kNext << column << kNext << old_state << kNext
      << new_state << kNext
      << AsHex::Address(map.is_null() ? kNullAddress : map->ptr()) << kNext;
  if (key->IsSmi()) {
    msg << Smi::ToInt(*key);
  } else if (key->IsNumber()) {
    msg << key->Number();
  } else if (key->IsName()) {
    msg << Name::cast(*key);
  }
  msg << kNext << modifier << kNext;
  if (slow_stub_reason != nullptr) {
    msg << slow_stub_reason;
  }
  msg.WriteToLogFile();
}

void V8FileLogger::MapEvent(const char* type, Handle<Map> from, Handle<Map> to,
                            const char* reason,
                            Handle<HeapObject> name_or_sfi) {
  if (!FLAG_log_maps) return;
  if (!to.is_null()) MapDetails(*to);
  int line = -1;
  int column = -1;
  Address pc = 0;

  if (!isolate_->bootstrapper()->IsActive()) {
    pc = isolate_->GetAbstractPC(&line, &column);
  }
  MSG_BUILDER();
  msg << "map" << kNext << type << kNext << Time() << kNext
      << AsHex::Address(from.is_null() ? kNullAddress : from->ptr()) << kNext
      << AsHex::Address(to.is_null() ? kNullAddress : to->ptr()) << kNext
      << AsHex::Address(pc) << kNext << line << kNext << column << kNext
      << reason << kNext;

  if (!name_or_sfi.is_null()) {
    if (name_or_sfi->IsName()) {
      msg << Name::cast(*name_or_sfi);
    } else if (name_or_sfi->IsSharedFunctionInfo()) {
      SharedFunctionInfo sfi = SharedFunctionInfo::cast(*name_or_sfi);
      msg << sfi.DebugNameCStr().get();
#if V8_SFI_HAS_UNIQUE_ID
      msg << " " << sfi.unique_id();
#endif  // V8_SFI_HAS_UNIQUE_ID
    }
  }
  msg.WriteToLogFile();
}

void V8FileLogger::MapCreate(Map map) {
  if (!FLAG_log_maps) return;
  DisallowGarbageCollection no_gc;
  MSG_BUILDER();
  msg << "map-create" << kNext << Time() << kNext << AsHex::Address(map.ptr());
  msg.WriteToLogFile();
}

void V8FileLogger::MapDetails(Map map) {
  if (!FLAG_log_maps) return;
  DisallowGarbageCollection no_gc;
  MSG_BUILDER();
  msg << "map-details" << kNext << Time() << kNext << AsHex::Address(map.ptr())
      << kNext;
  if (FLAG_log_maps_details) {
    std::ostringstream buffer;
    map.PrintMapDetails(buffer);
    msg << buffer.str().c_str();
  }
  msg.WriteToLogFile();
}

static std::vector<std::pair<Handle<SharedFunctionInfo>, Handle<AbstractCode>>>
EnumerateCompiledFunctions(Heap* heap) {
  HeapObjectIterator iterator(heap);
  DisallowGarbageCollection no_gc;
  std::vector<std::pair<Handle<SharedFunctionInfo>, Handle<AbstractCode>>>
      compiled_funcs;
  Isolate* isolate = heap->isolate();

  // Iterate the heap to find JSFunctions and record their optimized code.
  for (HeapObject obj = iterator.Next(); !obj.is_null();
       obj = iterator.Next()) {
    if (obj.IsSharedFunctionInfo()) {
      SharedFunctionInfo sfi = SharedFunctionInfo::cast(obj);
      if (sfi.is_compiled() && !sfi.HasBytecodeArray()) {
        compiled_funcs.emplace_back(
            handle(sfi, isolate),
            handle(AbstractCode::cast(sfi.abstract_code(isolate)), isolate));
      }
    } else if (obj.IsJSFunction()) {
      // Given that we no longer iterate over all optimized JSFunctions, we need
      // to take care of this here.
      JSFunction function = JSFunction::cast(obj);
      // TODO(jarin) This leaves out deoptimized code that might still be on the
      // stack. Also note that we will not log optimized code objects that are
      // only on a type feedback vector. We should make this mroe precise.
      if (function.HasAttachedOptimizedCode() &&
          Script::cast(function.shared().script()).HasValidSource()) {
        compiled_funcs.emplace_back(
            handle(function.shared(), isolate),
            handle(AbstractCode::cast(FromCodeT(function.code())), isolate));
      }
    }
  }

  Script::Iterator script_iterator(heap->isolate());
  for (Script script = script_iterator.Next(); !script.is_null();
       script = script_iterator.Next()) {
    if (!script.HasValidSource()) continue;

    SharedFunctionInfo::ScriptIterator sfi_iterator(heap->isolate(), script);
    for (SharedFunctionInfo sfi = sfi_iterator.Next(); !sfi.is_null();
         sfi = sfi_iterator.Next()) {
      if (sfi.is_compiled()) {
        compiled_funcs.emplace_back(
            handle(sfi, isolate),
            handle(AbstractCode::cast(sfi.abstract_code(isolate)), isolate));
      }
    }
  }

  return compiled_funcs;
}

void V8FileLogger::LogCodeObjects() { existing_code_logger_.LogCodeObjects(); }

void V8FileLogger::LogExistingFunction(Handle<SharedFunctionInfo> shared,
                                       Handle<AbstractCode> code) {
  existing_code_logger_.LogExistingFunction(shared, code);
}

void V8FileLogger::LogCompiledFunctions() {
  existing_code_logger_.LogCompiledFunctions();
}

void V8FileLogger::LogBuiltins() { existing_code_logger_.LogBuiltins(); }

void V8FileLogger::LogAccessorCallbacks() {
  Heap* heap = isolate_->heap();
  HeapObjectIterator iterator(heap);
  DisallowGarbageCollection no_gc;
  for (HeapObject obj = iterator.Next(); !obj.is_null();
       obj = iterator.Next()) {
    if (!obj.IsAccessorInfo()) continue;
    AccessorInfo ai = AccessorInfo::cast(obj);
    if (!ai.name().IsName()) continue;
    Address getter_entry = ai.getter();
    HandleScope scope(isolate_);
    Handle<Name> name(Name::cast(ai.name()), isolate_);
    if (getter_entry != kNullAddress) {
#if USES_FUNCTION_DESCRIPTORS
      getter_entry = *FUNCTION_ENTRYPOINT_ADDRESS(getter_entry);
#endif
      PROFILE(isolate_, GetterCallbackEvent(name, getter_entry));
    }
    Address setter_entry = ai.setter();
    if (setter_entry != kNullAddress) {
#if USES_FUNCTION_DESCRIPTORS
      setter_entry = *FUNCTION_ENTRYPOINT_ADDRESS(setter_entry);
#endif
      PROFILE(isolate_, SetterCallbackEvent(name, setter_entry));
    }
  }
}

void V8FileLogger::LogAllMaps() {
  Heap* heap = isolate_->heap();
  CombinedHeapObjectIterator iterator(heap);
  for (HeapObject obj = iterator.Next(); !obj.is_null();
       obj = iterator.Next()) {
    if (!obj.IsMap()) continue;
    Map map = Map::cast(obj);
    MapCreate(map);
    MapDetails(map);
  }
}

static void AddIsolateIdIfNeeded(std::ostream& os, Isolate* isolate) {
  if (!FLAG_logfile_per_isolate) return;
  os << "isolate-" << isolate << "-" << base::OS::GetCurrentProcessId() << "-";
}

static void PrepareLogFileName(std::ostream& os, Isolate* isolate,
                               const char* file_name) {
  int dir_separator_count = 0;
  for (const char* p = file_name; *p; p++) {
    if (base::OS::isDirectorySeparator(*p)) dir_separator_count++;
  }

  for (const char* p = file_name; *p; p++) {
    if (dir_separator_count == 0) {
      AddIsolateIdIfNeeded(os, isolate);
      dir_separator_count--;
    }
    if (*p == '%') {
      p++;
      switch (*p) {
        case '\0':
          // If there's a % at the end of the string we back up
          // one character so we can escape the loop properly.
          p--;
          break;
        case 'p':
          os << base::OS::GetCurrentProcessId();
          break;
        case 't':
          // %t expands to the current time in milliseconds.
          os << static_cast<int64_t>(
              V8::GetCurrentPlatform()->CurrentClockTimeMillis());
          break;
        case '%':
          // %% expands (contracts really) to %.
          os << '%';
          break;
        default:
          // All other %'s expand to themselves.
          os << '%' << *p;
          break;
      }
    } else {
      if (base::OS::isDirectorySeparator(*p)) dir_separator_count--;
      os << *p;
    }
  }
}

bool V8FileLogger::SetUp(Isolate* isolate) {
  // Tests and EnsureInitialize() can call this twice in a row. It's harmless.
  if (is_initialized_) return true;
  is_initialized_ = true;

  std::ostringstream log_file_name;
  PrepareLogFileName(log_file_name, isolate, FLAG_logfile);
  log_ = std::make_unique<LogFile>(this, log_file_name.str());

#if V8_OS_LINUX
  if (FLAG_perf_basic_prof) {
    perf_basic_logger_ = std::make_unique<LinuxPerfBasicLogger>(isolate);
    AddLogEventListener(perf_basic_logger_.get());
  }

  if (FLAG_perf_prof) {
    perf_jit_logger_ = std::make_unique<LinuxPerfJitLogger>(isolate);
    AddLogEventListener(perf_jit_logger_.get());
  }
#else
  static_assert(
      !FLAG_perf_prof.value(),
      "--perf-prof should be statically disabled on non-Linux platforms");
  static_assert(
      !FLAG_perf_basic_prof.value(),
      "--perf-basic-prof should be statically disabled on non-Linux platforms");
#endif

#ifdef ENABLE_GDB_JIT_INTERFACE
  if (i::FLAG_gdbjit) {
    gdb_jit_logger_ =
        std::make_unique<JitLogger>(isolate, i::GDBJITInterface::EventHandler);
    AddLogEventListener(gdb_jit_logger_.get());
    CHECK(isolate->logger()->is_listening_to_code_events());
  }
#endif  // ENABLE_GDB_JIT_INTERFACE

#if defined(V8_OS_WIN) && defined(V8_ENABLE_ETW_STACK_WALKING)
  if (i::FLAG_enable_etw_stack_walking) {
    etw_jit_logger_ =
        std::make_unique<JitLogger>(isolate, i::ETWJITInterface::EventHandler);
    AddLogEventListener(etw_jit_logger_.get());
    CHECK(isolate->logger()->is_listening_to_code_events());
  }
#endif  // defined(V8_OS_WIN)

  if (FLAG_ll_prof) {
    ll_logger_ =
        std::make_unique<LowLevelLogger>(isolate, log_file_name.str().c_str());
    AddLogEventListener(ll_logger_.get());
  }
  ticker_ = std::make_unique<Ticker>(isolate, FLAG_prof_sampling_interval);
  if (FLAG_log) UpdateIsLogging(true);
  timer_.Start();
  if (FLAG_prof_cpp) {
    CHECK(FLAG_log);
    CHECK(is_logging());
    profiler_ = std::make_unique<Profiler>(isolate);
    profiler_->Engage();
  }
  if (is_logging_) AddLogEventListener(this);
  return true;
}

void V8FileLogger::LateSetup(Isolate* isolate) {
  if (!isolate->logger()->is_listening_to_code_events()) return;
  Builtins::EmitCodeCreateEvents(isolate);
#if V8_ENABLE_WEBASSEMBLY
  wasm::GetWasmEngine()->EnableCodeLogging(isolate);
#endif
}

void V8FileLogger::SetCodeEventHandler(uint32_t options,
                                       JitCodeEventHandler event_handler) {
  if (jit_logger_) {
    RemoveLogEventListener(jit_logger_.get());
    jit_logger_.reset();
    isolate_->UpdateLogObjectRelocation();
  }

  if (event_handler) {
#if V8_ENABLE_WEBASSEMBLY
    wasm::GetWasmEngine()->EnableCodeLogging(isolate_);
#endif  // V8_ENABLE_WEBASSEMBLY
    jit_logger_ = std::make_unique<JitLogger>(isolate_, event_handler);
    isolate_->UpdateLogObjectRelocation();
    AddLogEventListener(jit_logger_.get());
    if (options & kJitCodeEventEnumExisting) {
      HandleScope scope(isolate_);
      LogBuiltins();
      LogCodeObjects();
      LogCompiledFunctions();
    }
  }
}

sampler::Sampler* V8FileLogger::sampler() { return ticker_.get(); }
std::string V8FileLogger::file_name() const { return log_.get()->file_name(); }

void V8FileLogger::StopProfilerThread() {
  if (profiler_ != nullptr) {
    profiler_->Disengage();
    profiler_.reset();
  }
}

FILE* V8FileLogger::TearDownAndGetLogFile() {
  if (!is_initialized_) return nullptr;
  is_initialized_ = false;
  UpdateIsLogging(false);

  // Stop the profiler thread before closing the file.
  StopProfilerThread();

  ticker_.reset();
  timer_.Stop();

#if V8_OS_LINUX
  if (perf_basic_logger_) {
    RemoveLogEventListener(perf_basic_logger_.get());
    perf_basic_logger_.reset();
  }

  if (perf_jit_logger_) {
    RemoveLogEventListener(perf_jit_logger_.get());
    perf_jit_logger_.reset();
  }
#endif

  if (ll_logger_) {
    RemoveLogEventListener(ll_logger_.get());
    ll_logger_.reset();
  }

  if (jit_logger_) {
    RemoveLogEventListener(jit_logger_.get());
    jit_logger_.reset();
    isolate_->UpdateLogObjectRelocation();
  }

  return log_->Close();
}

void V8FileLogger::UpdateIsLogging(bool value) {
  base::MutexGuard guard(log_->mutex());
  if (value) {
    isolate_->CollectSourcePositionsForAllBytecodeArrays();
  }
  // Relaxed atomic to avoid locking the mutex for the most common case: when
  // logging is disabled.
  is_logging_.store(value, std::memory_order_relaxed);
  isolate_->UpdateLogObjectRelocation();
}

void ExistingCodeLogger::LogCodeObject(AbstractCode object) {
  HandleScope scope(isolate_);
  Handle<AbstractCode> abstract_code(object, isolate_);
  CodeTag tag = CodeTag::kStub;
  const char* description = "Unknown code from before profiling";
  PtrComprCageBase cage_base(isolate_);
  switch (abstract_code->kind(cage_base)) {
    case CodeKind::INTERPRETED_FUNCTION:
    case CodeKind::TURBOFAN:
    case CodeKind::BASELINE:
    case CodeKind::MAGLEV:
      return;  // We log this later using LogCompiledFunctions.
    case CodeKind::FOR_TESTING:
      description = "STUB code";
      tag = CodeTag::kStub;
      break;
    case CodeKind::REGEXP:
      description = "Regular expression code";
      tag = CodeTag::kRegExp;
      break;
    case CodeKind::BYTECODE_HANDLER:
      description =
          isolate_->builtins()->name(abstract_code->builtin_id(cage_base));
      tag = CodeTag::kBytecodeHandler;
      break;
    case CodeKind::BUILTIN:
      if (!abstract_code->is_off_heap_trampoline(cage_base)) {
        DCHECK_EQ(abstract_code->builtin_id(cage_base),
                  Builtin::kInterpreterEntryTrampoline);
        // We treat interpreter trampoline builtin copies as
        // INTERPRETED_FUNCTION, which are logged using LogCompiledFunctions.
        return;
      }
      description = Builtins::name(abstract_code->builtin_id(cage_base));
      tag = CodeTag::kBuiltin;
      break;
    case CodeKind::WASM_FUNCTION:
      description = "A Wasm function";
      tag = CodeTag::kFunction;
      break;
    case CodeKind::JS_TO_WASM_FUNCTION:
      description = "A JavaScript to Wasm adapter";
      tag = CodeTag::kStub;
      break;
    case CodeKind::JS_TO_JS_FUNCTION:
      description = "A WebAssembly.Function adapter";
      tag = CodeTag::kStub;
      break;
    case CodeKind::WASM_TO_CAPI_FUNCTION:
      description = "A Wasm to C-API adapter";
      tag = CodeTag::kStub;
      break;
    case CodeKind::WASM_TO_JS_FUNCTION:
      description = "A Wasm to JavaScript adapter";
      tag = CodeTag::kStub;
      break;
    case CodeKind::C_WASM_ENTRY:
      description = "A C to Wasm entry stub";
      tag = CodeTag::kStub;
      break;
  }
  CALL_CODE_EVENT_HANDLER(CodeCreateEvent(tag, abstract_code, description))
}

void ExistingCodeLogger::LogCodeObjects() {
  Heap* heap = isolate_->heap();
  HeapObjectIterator iterator(heap);
  DisallowGarbageCollection no_gc;
  PtrComprCageBase cage_base(isolate_);
  for (HeapObject obj = iterator.Next(); !obj.is_null();
       obj = iterator.Next()) {
    InstanceType instance_type = obj.map(cage_base).instance_type();
    if (V8_REMOVE_BUILTINS_CODE_OBJECTS) {
      // In this case AbstactCode is Code|CodeDataContainer|BytecodeArray but
      // we want to log code objects only once, thus we ignore Code objects
      // which will be logged via corresponding CodeDataContainer.
      if (InstanceTypeChecker::IsCodeT(instance_type) ||
          InstanceTypeChecker::IsBytecodeArray(instance_type)) {
        LogCodeObject(AbstractCode::cast(obj));
      }
    } else {
      // In this case AbstactCode is Code|BytecodeArray.
      if (InstanceTypeChecker::IsCode(instance_type) ||
          InstanceTypeChecker::IsBytecodeArray(instance_type)) {
        LogCodeObject(AbstractCode::cast(obj));
      }
    }
  }
}

void ExistingCodeLogger::LogBuiltins() {
  DCHECK(isolate_->builtins()->is_initialized());
  // The main "copy" of used builtins are logged by LogCodeObjects() while
  // iterating CodeT objects.
  // TODO(v8:11880): Log other copies of remapped builtins once we
  // decide to remap them multiple times into the code range (for example
  // for arm64).
}

void ExistingCodeLogger::LogCompiledFunctions() {
  Heap* heap = isolate_->heap();
  HandleScope scope(isolate_);
  std::vector<std::pair<Handle<SharedFunctionInfo>, Handle<AbstractCode>>>
      compiled_funcs = EnumerateCompiledFunctions(heap);

  // During iteration, there can be heap allocation due to
  // GetScriptLineNumber call.
  for (auto& pair : compiled_funcs) {
    Handle<SharedFunctionInfo> shared = pair.first;
    SharedFunctionInfo::EnsureSourcePositionsAvailable(isolate_, shared);
    if (shared->HasInterpreterData()) {
      LogExistingFunction(
          shared,
          Handle<AbstractCode>(
              AbstractCode::cast(FromCodeT(shared->InterpreterTrampoline())),
              isolate_));
    }
    if (shared->HasBaselineCode()) {
      LogExistingFunction(shared, Handle<AbstractCode>(
                                      AbstractCode::cast(FromCodeT(
                                          shared->baseline_code(kAcquireLoad))),
                                      isolate_));
    }
    // Can't use .is_identical_to() because AbstractCode might be both Code and
    // non-Code object and regular tagged comparison or compressed values might
    // not be correct when V8_EXTERNAL_CODE_SPACE is enabled.
    if (*pair.second == *BUILTIN_CODE(isolate_, CompileLazy)) continue;
    LogExistingFunction(pair.first, pair.second);
  }

#if V8_ENABLE_WEBASSEMBLY
  HeapObjectIterator iterator(heap);
  DisallowGarbageCollection no_gc;

  for (HeapObject obj = iterator.Next(); !obj.is_null();
       obj = iterator.Next()) {
    if (!obj.IsWasmModuleObject()) continue;
    auto module_object = WasmModuleObject::cast(obj);
    module_object.native_module()->LogWasmCodes(isolate_,
                                                module_object.script());
  }
#endif  // V8_ENABLE_WEBASSEMBLY
}

void ExistingCodeLogger::LogExistingFunction(Handle<SharedFunctionInfo> shared,
                                             Handle<AbstractCode> code,
                                             CodeTag tag) {
  if (shared->script().IsScript()) {
    Handle<Script> script(Script::cast(shared->script()), isolate_);
    int line_num = Script::GetLineNumber(script, shared->StartPosition()) + 1;
    int column_num =
        Script::GetColumnNumber(script, shared->StartPosition()) + 1;
    if (script->name().IsString()) {
      Handle<String> script_name(String::cast(script->name()), isolate_);
      if (!shared->is_toplevel()) {
        CALL_CODE_EVENT_HANDLER(
            CodeCreateEvent(V8FileLogger::ToNativeByScript(tag, *script), code,
                            shared, script_name, line_num, column_num))
      } else {
        // Can't distinguish eval and script here, so always use Script.
        CALL_CODE_EVENT_HANDLER(CodeCreateEvent(
            V8FileLogger::ToNativeByScript(CodeTag::kScript, *script), code,
            shared, script_name))
      }
    } else {
      CALL_CODE_EVENT_HANDLER(CodeCreateEvent(
          V8FileLogger::ToNativeByScript(tag, *script), code, shared,
          ReadOnlyRoots(isolate_).empty_string_handle(), line_num, column_num))
    }
  } else if (shared->IsApiFunction()) {
    // API function.
    Handle<FunctionTemplateInfo> fun_data =
        handle(shared->get_api_func_data(), isolate_);
    Object raw_call_data = fun_data->call_code(kAcquireLoad);
    if (!raw_call_data.IsUndefined(isolate_)) {
      CallHandlerInfo call_data = CallHandlerInfo::cast(raw_call_data);
      Address entry_point = call_data.callback();
#if USES_FUNCTION_DESCRIPTORS
      entry_point = *FUNCTION_ENTRYPOINT_ADDRESS(entry_point);
#endif
      Handle<String> fun_name = SharedFunctionInfo::DebugName(shared);
      CALL_CODE_EVENT_HANDLER(CallbackEvent(fun_name, entry_point))

      // Fast API function.
      int c_functions_count = fun_data->GetCFunctionsCount();
      for (int i = 0; i < c_functions_count; i++) {
        CALL_CODE_EVENT_HANDLER(
            CallbackEvent(fun_name, fun_data->GetCFunction(i)))
      }
    }
  }
}

#undef CALL_CODE_EVENT_HANDLER
#undef MSG_BUILDER

}  // namespace internal
}  // namespace v8
