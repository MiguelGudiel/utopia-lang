/*
 * Utopia Runtime
 *
 * Powers Utopia's C++-style exception handling (try/catch/throw) and the
 * 'assert' statement. It is a small, self-contained C++ library with no
 * dependencies beyond libgcc's unwinder (_Unwind_* from <unwind.h>), so it
 * links with a plain C linker.
 *
 * Exception model
 * ---------------
 * - 'throw expr' lowers to: utopia_allocate_exception(size) -> copy the
 *   value into the returned storage -> utopia_throw(valuePtr, typeInfo,
 *   dtor). utopia_throw raises the exception with _Unwind_RaiseException.
 * - The thrown value is stored in a UtopiaException record whose first
 *   member is the _Unwind_Exception header. 'typeInfo' is a compiler-
 *   generated descriptor (the same _ZTI globals that back 'expr is T'):
 *   an array [parentDescriptor, interfaceDescriptors..., null].
 * - Catch matching is performed by the personality function (SEARCH phase)
 *   against the landing pad's type table in the LSDA. A catch clause
 *   matches when its descriptor equals the thrown descriptor or one of
 *   its ancestors/interfaces, so any type can be thrown and caught
 *   (catch-all via 'catch (...)').
 * - utopia_begin_catch/utopia_end_catch manage the exception's lifetime:
 *   the value's destructor runs and the record is freed when the last
 *   catch exits. utopia_rethrow marks the catch as rethrowing and raises
 *   the same exception object again (preserving its dynamic type).
 * - If no handler exists, utopia_throw/utopia_rethrow terminate the
 *   process (like std::terminate in C++).
 */

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unwind.h>

namespace {

/* Signature identifying exceptions raised by this runtime. */
constexpr uint64_t kUtopiaExceptionClass =
    (uint64_t)'U' << 56 | (uint64_t)'T' << 48 | (uint64_t)'O' << 40 |
    (uint64_t)'P' << 32 | (uint64_t)'I' << 24 | (uint64_t)'A' << 16;

/* DWARF pointer-encoding constants (dwarf2.h, kept local on purpose). */
constexpr uint8_t DW_EH_PE_absptr = 0x00;
constexpr uint8_t DW_EH_PE_omit = 0xff;
constexpr uint8_t DW_EH_PE_uleb128 = 0x01;
constexpr uint8_t DW_EH_PE_udata2 = 0x02;
constexpr uint8_t DW_EH_PE_udata4 = 0x03;
constexpr uint8_t DW_EH_PE_udata8 = 0x04;
constexpr uint8_t DW_EH_PE_sleb128 = 0x09;
constexpr uint8_t DW_EH_PE_sdata2 = 0x0A;
constexpr uint8_t DW_EH_PE_sdata4 = 0x0B;
constexpr uint8_t DW_EH_PE_sdata8 = 0x0C;
constexpr uint8_t DW_EH_PE_pcrel = 0x10;
constexpr uint8_t DW_EH_PE_textrel = 0x20;
constexpr uint8_t DW_EH_PE_datarel = 0x30;
constexpr uint8_t DW_EH_PE_funcrel = 0x40;
constexpr uint8_t DW_EH_PE_indirect = 0x80;

/*
 * The exception record. The value storage immediately follows the struct,
 * aligned to max_align_t. The compiler never computes these offsets: it
 * only passes the exception pointer (or the value pointer obtained from
 * utopia_allocate_exception) to the runtime functions below.
 */
struct alignas(std::max_align_t) UtopiaException {
  _Unwind_Exception unwindHeader;
  void *typeInfo = nullptr;
  void (*valueDtor)(void *) = nullptr;
  /* Size of the thrown value in bytes, recorded at allocation time. The
   * async runtime uses it to copy the value out of the record (see
   * utopia_exception_info). */
  uint64_t valueSize = 0;
  intptr_t handlerCount = 0;
  /* Phase-1 bookkeeping, restored in phase 2. */
  intptr_t matchedSelector = -1;
  uintptr_t matchedLandingPad = 0;
  unsigned char valueStorage[];
};

UtopiaException *exceptionFromValue(void *valuePtr) {
  return reinterpret_cast<UtopiaException *>(
      reinterpret_cast<char *>(valuePtr) -
      offsetof(UtopiaException, valueStorage));
}

void *valueFromException(UtopiaException *exn) {
  return reinterpret_cast<char *>(exn) +
         offsetof(UtopiaException, valueStorage);
}

/* Called by the unwinder when an exception object is deleted with
 * _Unwind_DeleteException. Utopia never calls that function; the cleanup
 * exists for ABI completeness and only releases the record. */
void exceptionCleanup(_Unwind_Reason_Code, _Unwind_Exception *exnHeader) {
  std::free(exnHeader);
}

/* ------------------------------------------------------------------ */
/* LSDA (language-specific data area) decoding.                        */
/* ------------------------------------------------------------------ */

struct LsdaInfo {
  uintptr_t start = 0;   /* function start (@LPStart when omitted) */
  uintptr_t lpStart = 0; /* landing-pad base */
  const unsigned char *ttype = nullptr; /* one past the type table */
  unsigned char ttypeEncoding = 0;
  unsigned char callSiteEncoding = 0;
  const unsigned char *actionTable = nullptr;
};

const unsigned char *readUleb128(const unsigned char *p, uint64_t &out) {
  uint64_t result = 0;
  unsigned shift = 0;
  uint8_t byte;
  do {
    byte = *p++;
    result |= uint64_t(byte & 0x7f) << shift;
    shift += 7;
  } while (byte & 0x80);
  out = result;
  return p;
}

const unsigned char *readSleb128(const unsigned char *p, int64_t &out) {
  uint64_t result = 0;
  unsigned shift = 0;
  uint8_t byte;
  do {
    byte = *p++;
    result |= uint64_t(byte & 0x7f) << shift;
    shift += 7;
  } while (byte & 0x80);
  if (shift < 64 && (byte & 0x40))
    result |= ~uint64_t(0) << shift;
  out = static_cast<int64_t>(result);
  return p;
}

uintptr_t baseOfEncodedValue(unsigned char encoding,
                             _Unwind_Context *context) {
  switch (encoding & 0x70) {
  case DW_EH_PE_textrel:
    return reinterpret_cast<uintptr_t>(_Unwind_GetTextRelBase(context));
  case DW_EH_PE_datarel:
    return reinterpret_cast<uintptr_t>(_Unwind_GetDataRelBase(context));
  case DW_EH_PE_funcrel:
    return reinterpret_cast<uintptr_t>(_Unwind_GetRegionStart(context));
  default:
    return 0;
  }
}

/* Reads one pointer-encoded value, following libgcc's unwind-pe.h
 * semantics: pcrel values are relative to the value's own position, other
 * relative encodings to their base; DW_EH_PE_indirect dereferences the
 * result. */
uintptr_t readEncodedValue(_Unwind_Context *context, unsigned char encoding,
                           const unsigned char *p, const unsigned char *&end) {
  const uintptr_t startPos = reinterpret_cast<uintptr_t>(p);
  uintptr_t raw = 0;

  switch (encoding & 0x0f) {
  case DW_EH_PE_absptr:
    std::memcpy(&raw, p, sizeof(void *));
    p += sizeof(void *);
    break;
  case DW_EH_PE_uleb128: {
    uint64_t v;
    p = readUleb128(p, v);
    raw = static_cast<uintptr_t>(v);
    break;
  }
  case DW_EH_PE_sleb128: {
    int64_t v;
    p = readSleb128(p, v);
    raw = static_cast<uintptr_t>(v);
    break;
  }
  case DW_EH_PE_udata2: {
    uint16_t v;
    std::memcpy(&v, p, 2);
    raw = v;
    p += 2;
    break;
  }
  case DW_EH_PE_sdata2: {
    int16_t v;
    std::memcpy(&v, p, 2);
    raw = static_cast<uintptr_t>(static_cast<intptr_t>(v));
    p += 2;
    break;
  }
  case DW_EH_PE_udata4: {
    uint32_t v;
    std::memcpy(&v, p, 4);
    raw = v;
    p += 4;
    break;
  }
  case DW_EH_PE_sdata4: {
    int32_t v;
    std::memcpy(&v, p, 4);
    raw = static_cast<uintptr_t>(static_cast<intptr_t>(v));
    p += 4;
    break;
  }
  case DW_EH_PE_udata8: {
    uint64_t v;
    std::memcpy(&v, p, 8);
    raw = static_cast<uintptr_t>(v);
    p += 8;
    break;
  }
  case DW_EH_PE_sdata8: {
    int64_t v;
    std::memcpy(&v, p, 8);
    raw = static_cast<uintptr_t>(v);
    p += 8;
    break;
  }
  default:
    std::abort();
  }
  end = p;

  /* A raw zero value stays null (the catch-all entry): the relative base
   * is only added and DW_EH_PE_indirect only dereferences for non-zero
   * values, mirroring libgcc's read_encoded_value_with_base. */
  if (raw == 0)
    return 0;

  uintptr_t result;
  if ((encoding & 0x70) == DW_EH_PE_pcrel) {
    result = raw + startPos;
  } else if (encoding & 0x70) {
    result = raw + baseOfEncodedValue(encoding, context);
  } else {
    result = raw;
  }

  if (encoding & DW_EH_PE_indirect)
    std::memcpy(&result, reinterpret_cast<void *>(result), sizeof(void *));
  return result;
}

const unsigned char *parseLsda(_Unwind_Context *context,
                               const unsigned char *lsda, LsdaInfo &info) {
  const unsigned char *p = lsda;

  info.start = reinterpret_cast<uintptr_t>(_Unwind_GetRegionStart(context));

  unsigned char lpStartEncoding = *p++;
  if (lpStartEncoding != DW_EH_PE_omit) {
    const unsigned char *end;
    info.lpStart = readEncodedValue(context, lpStartEncoding, p, end);
    p = end;
  } else {
    info.lpStart = info.start;
  }

  info.ttypeEncoding = *p++;
  if (info.ttypeEncoding != DW_EH_PE_omit) {
    uint64_t ttypeSize;
    p = readUleb128(p, ttypeSize);
    info.ttype = p + ttypeSize;
  } else {
    info.ttype = nullptr;
  }

  info.callSiteEncoding = *p++;
  uint64_t csSize;
  p = readUleb128(p, csSize);
  info.actionTable = p + csSize;

  return p; /* start of the call-site table */
}

unsigned entrySizeFor(unsigned char encoding) {
  switch (encoding & 0x0f) {
  case DW_EH_PE_absptr:
    return sizeof(void *);
  case DW_EH_PE_udata2:
  case DW_EH_PE_sdata2:
    return 2;
  case DW_EH_PE_udata4:
  case DW_EH_PE_sdata4:
    return 4;
  case DW_EH_PE_udata8:
  case DW_EH_PE_sdata8:
    return 8;
  default:
    std::abort();
  }
}

/* Type-table entry 'i' (1-based). The table is emitted in reverse order,
 * so entry i lives at 'ttype - i * size'. */
uintptr_t getTypeTableEntry(_Unwind_Context *context, const LsdaInfo &info,
                            uintptr_t index) {
  const unsigned char *p =
      info.ttype - index * entrySizeFor(info.ttypeEncoding);
  const unsigned char *end;
  return readEncodedValue(context, info.ttypeEncoding, p, end);
}

/*
 * True when 'catchType' is the same descriptor as 'thrownType' or an
 * ancestor / interface of it. Descriptors are compiler-generated arrays:
 * slot 0 is the parent descriptor (or null), slots 1..n are interface
 * descriptors, terminated by null.
 */
bool typeMatches(uintptr_t catchType, uintptr_t thrownType) {
  if (catchType == thrownType)
    return true;

  uintptr_t current = thrownType;
  while (current != 0) {
    uintptr_t *descriptor = reinterpret_cast<uintptr_t *>(current);
    for (uintptr_t iface, i = 1; (iface = descriptor[i]) != 0; ++i) {
      if (typeMatches(catchType, iface))
        return true;
    }
    uintptr_t parent = descriptor[0];
    if (parent == catchType)
      return true;
    current = parent;
  }
  return false;
}

/*
 * Walks the landing pad's action records for the current call site.
 * Returns the matched selector (0 when the catch-all matched) and whether
 * the pad has a pure cleanup clause. In the cleanup phase handler filters
 * are skipped (they were checked in the search phase).
 */
struct ActionResult {
  intptr_t selector = -1; /* -1: no handler matched */
  bool hasCleanup = false;
};

ActionResult matchActions(_Unwind_Context *context, const LsdaInfo &info,
                          const unsigned char *actionRecord, void *thrownType,
                          bool cleanupPhase) {
  ActionResult result;

  /* The records chain through their signed 'next' offsets; a zero 'next'
   * ends the chain. */
  while (actionRecord) {
    const unsigned char *p = actionRecord;
    int64_t filter, next;
    p = readSleb128(p, filter);
    /* The 'next' offset is relative to the position after the filter
     * value, not after the whole record. */
    const unsigned char *afterFilter = p;
    readSleb128(p, next);

    if (filter == 0) {
      /* Zero filters mark cleanups. */
      result.hasCleanup = true;
    } else if (cleanupPhase) {
      /* Handlers were already checked in the search phase. */
    } else if (filter > 0) {
      uintptr_t catchType = getTypeTableEntry(context, info, filter);
      if (catchType == 0) {
        /* Null type table entry: catch-all. */
        result.selector = 0;
        break;
      }
      if (typeMatches(catchType, reinterpret_cast<uintptr_t>(thrownType))) {
        result.selector = filter;
        break;
      }
    }

    if (next == 0)
      break;
    actionRecord = afterFilter + next;
  }
  return result;
}

} // namespace

/* ------------------------------------------------------------------ */
/* Public runtime API (extern "C").                                    */
/* ------------------------------------------------------------------ */

extern "C" {

void utopia_end_catch(void *exnPtr);

/* Allocates a fresh exception record sized for a value of 'size' bytes and
 * returns the pointer to the value storage. Never returns null. The header
 * is zero-initialized so disposal paths that never raise (e.g. the async
 * timeout) never read a garbage destructor or type descriptor. */
void *utopia_allocate_exception(uint64_t size) {
  void *block = std::calloc(1, sizeof(UtopiaException) + size);
  if (!block)
    std::abort();
  static_cast<UtopiaException *>(block)->valueSize = size;
  return valueFromException(static_cast<UtopiaException *>(block));
}

/* Exposes the thrown value stored in the exception record: the type
 * descriptor, the value pointer, its size and its destructor (null when
 * trivially destructible). The async runtime copies the value out of the
 * record with these fields; the record itself remains owned by the
 * current catch handler. */
void utopia_exception_info(void *exnPtr, void **outTypeInfo,
                           void **outValuePtr, uint64_t *outValueSize,
                           void (**outDtor)(void *)) {
  UtopiaException *exn = static_cast<UtopiaException *>(exnPtr);
  *outTypeInfo = exn->typeInfo;
  *outValuePtr = valueFromException(exn);
  *outValueSize = exn->valueSize;
  *outDtor = exn->valueDtor;
}

/* Destroys the value and frees the record of an exception that was never
 * raised (e.g. one whose value was copied into a Future's error slot by
 * the async runtime). Takes the value pointer returned by
 * utopia_allocate_exception, like utopia_throw does. The record must not
 * be referenced by any active handler. */
void utopia_exception_dispose(void *valuePtr) {
  UtopiaException *exn = exceptionFromValue(valuePtr);
  if (exn->valueDtor)
    exn->valueDtor(valuePtr);
  std::free(exn);
}

/* Raises the exception stored at 'valuePtr'. The value was already copied
 * into the storage returned by utopia_allocate_exception; 'typeInfo' is
 * the thrown type's descriptor and 'dtor' destroys the value (or null for
 * trivially destructible types). Does not return. */
void utopia_throw(void *valuePtr, void *typeInfo, void (*dtor)(void *)) {
  UtopiaException *exn = exceptionFromValue(valuePtr);
  exn->unwindHeader.exception_class = kUtopiaExceptionClass;
  exn->unwindHeader.exception_cleanup = exceptionCleanup;
  exn->typeInfo = typeInfo;
  exn->valueDtor = dtor;
  exn->handlerCount = 0;
  exn->matchedSelector = -1;

  _Unwind_RaiseException(&exn->unwindHeader); /* only returns on failure */

  if (exn->valueDtor)
    exn->valueDtor(valuePtr);
  std::free(exn);
  std::fputs("Unhandled exception: no matching catch clause found.\n",
             stderr);
  std::abort();
}

/* Re-raises the exception currently being handled ('throw;' inside a
 * catch). The same exception object is raised again so its dynamic type
 * is preserved. Does not return. */
void utopia_rethrow(void *exnPtr) {
  UtopiaException *exn = static_cast<UtopiaException *>(exnPtr);
  exn->handlerCount = -exn->handlerCount;
  exn->matchedSelector = -1;

  _Unwind_RaiseException(&exn->unwindHeader); /* only returns on failure */

  utopia_end_catch(exnPtr);
  std::fputs("Unhandled exception: rethrow found no matching catch "
             "clause.\n",
             stderr);
  std::abort();
}

/* Enters a catch handler for the exception 'exnPtr'. Returns the pointer
 * to the thrown value storage, which the compiler copies from (by value)
 * or binds a reference to. */
void *utopia_begin_catch(void *exnPtr) {
  UtopiaException *exn = static_cast<UtopiaException *>(exnPtr);
  if (exn->handlerCount < 0)
    exn->handlerCount = -exn->handlerCount;
  exn->handlerCount++;
  return valueFromException(exn);
}

/* Leaves a catch handler. The exception's value is destroyed and the
 * record freed when the last handler exits; a pending rethrow keeps both
 * alive. */
void utopia_end_catch(void *exnPtr) {
  UtopiaException *exn = static_cast<UtopiaException *>(exnPtr);
  if (exn->handlerCount < 0) {
    exn->handlerCount = -exn->handlerCount;
    return;
  }
  if (--exn->handlerCount == 0) {
    void *valuePtr = valueFromException(exn);
    if (exn->valueDtor)
      exn->valueDtor(valuePtr);
    std::free(exn);
  }
}

/* Installs the context for a landing pad: sets the exception data
 * registers (the exception pointer and the matched selector, 0 for
 * cleanups) and jumps to the pad. */
static inline _Unwind_Reason_Code installContext(
    struct _Unwind_Exception *exceptionObject, UtopiaException *exn,
    struct _Unwind_Context *context, uintptr_t landingPad,
    bool isHandlerFrame) {
  _Unwind_SetGR(context, __builtin_eh_return_data_regno(0),
                reinterpret_cast<uintptr_t>(exceptionObject));
  _Unwind_SetGR(context, __builtin_eh_return_data_regno(1),
                static_cast<uintptr_t>(exn->matchedSelector >= 0
                                           ? exn->matchedSelector
                                           : 0));
  _Unwind_SetIP(context, isHandlerFrame ? exn->matchedLandingPad
                                        : landingPad);
  return _URC_INSTALL_CONTEXT;
}

/* The personality routine installed on every function that can unwind. It
 * implements the Itanium ABI's phase 1 (search) / phase 2 (cleanup)
 * protocol: in the search phase it walks the landing pad's action records
 * and matches the thrown descriptor against the catch types; in phase 2
 * it installs the context for the handler or a cleanup pad. */
_Unwind_Reason_Code utopia_personality(
    int version, _Unwind_Action actions,
    _Unwind_Exception_Class exceptionClass,
    struct _Unwind_Exception *exceptionObject,
    struct _Unwind_Context *context) {
  if (version != 1)
    return _URC_FATAL_PHASE1_ERROR;

  /* Foreign exceptions (raised by other runtimes) cannot be matched by
   * Utopia descriptors: let them propagate without running our cleanups. */
  if (exceptionClass != kUtopiaExceptionClass)
    return _URC_CONTINUE_UNWIND;

  UtopiaException *exn =
      reinterpret_cast<UtopiaException *>(exceptionObject);

  const unsigned char *lsda = static_cast<const unsigned char *>(
      _Unwind_GetLanguageSpecificData(context));
  if (!lsda)
    return _URC_CONTINUE_UNWIND;

  LsdaInfo info;
  const unsigned char *p = parseLsda(context, lsda, info);

  int ipBeforeInsn = 0;
  uintptr_t ip = static_cast<uintptr_t>(
      _Unwind_GetIPInfo(context, &ipBeforeInsn));
  if (!ipBeforeInsn)
    --ip;

  /* Phase-2 shortcut for the frame whose handler matched in phase 1. */
  if (actions == (_UA_CLEANUP_PHASE | _UA_HANDLER_FRAME) &&
      exn->matchedSelector >= 0) {
    return installContext(exceptionObject, exn, context,
                          exn->matchedLandingPad, true);
  }

  /* Locate the call-site entry covering the current IP. */
  const unsigned char *callSite = p;
  uintptr_t landingPad = 0;
  intptr_t action = 0;
  bool foundSite = false;

  while (callSite < info.actionTable) {
    const unsigned char *end;
    uintptr_t csStart =
        readEncodedValue(context, info.callSiteEncoding, callSite, end);
    callSite = end;
    uintptr_t csLen =
        readEncodedValue(context, info.callSiteEncoding, callSite, end);
    callSite = end;
    uintptr_t csLp =
        readEncodedValue(context, info.callSiteEncoding, callSite, end);
    callSite = end;
    uint64_t csAction;
    callSite = readUleb128(callSite, csAction);

    /* The table is sorted; once we pass the IP there is no match. */
    if (ip < info.start + csStart)
      break;
    if (ip < info.start + csStart + csLen) {
      foundSite = true;
      if (csLp)
        landingPad = info.lpStart + csLp;
      if (csAction)
        action = static_cast<intptr_t>(csAction);
      break;
    }
  }

  if (!foundSite) {
    /* The IP is not covered by the table (e.g. an exception raised from
     * a site the compiler did not expect to unwind): terminate. */
    std::fputs("Terminate called from an exception raised at an "
               "unexpected site.\n",
               stderr);
    std::abort();
  }

  if (landingPad == 0)
    return _URC_CONTINUE_UNWIND; /* nothing to clean or catch here */

  const unsigned char *actionRecord = nullptr;
  if (action != 0)
    actionRecord = info.actionTable + action - 1;

  if (actionRecord == nullptr) {
    /* Cleanup-only landing pad. */
    if (actions & _UA_SEARCH_PHASE)
      return _URC_CONTINUE_UNWIND;
    return installContext(exceptionObject, exn, context, landingPad, false);
  }

  ActionResult match =
      matchActions(context, info, actionRecord, exn->typeInfo,
                   (actions & _UA_SEARCH_PHASE) == 0);

  if (actions & _UA_SEARCH_PHASE) {
    if (match.selector >= 0) {
      exn->matchedSelector = match.selector;
      exn->matchedLandingPad = landingPad;
      return _URC_HANDLER_FOUND;
    }
    return _URC_CONTINUE_UNWIND;
  }

  if (match.hasCleanup || match.selector >= 0 ||
      exn->matchedSelector >= 0) {
    return installContext(exceptionObject, exn, context, landingPad,
                          (actions & _UA_HANDLER_FRAME) != 0);
  }

  return _URC_CONTINUE_UNWIND;
}

/* The 'assert' failure handler: prints the source location and aborts. */
void utopia_assert_failed(const char *file, int line, const char *message) {
  if (message && message[0]) {
    std::fprintf(stderr, "assertion failed: %s at %s:%d\n", message,
                 file ? file : "?", line);
  } else {
    std::fprintf(stderr, "assertion failed at %s:%d\n", file ? file : "?",
                 line);
  }
  std::abort();
}

/* ------------------------------------------------------------------ */
/* Command-line arguments for Env.args (stdlib system module).         */
/* ------------------------------------------------------------------ */

/* The compiler-emitted main wrapper stores the C runtime's argc/argv here
 * before calling the user's main, so Env.args() works even for a main that
 * declares no parameters. */
static int32_t utopia_args_count = 0;
static char **utopia_args_vector = nullptr;

void utopia_set_args(int32_t argc, char **argv) {
  utopia_args_count = argc;
  utopia_args_vector = argv;
}

int32_t utopia_get_argc() { return utopia_args_count; }

char **utopia_get_argv() { return utopia_args_vector; }

/* ------------------------------------------------------------------ */
/* String.format (prelude String module).                             */
/* ------------------------------------------------------------------ */

/* printf-style formatting for String.format(fmt, ...). Formats into a
 * persistent last-result buffer that is reused (and grown) by the next
 * call, like asctime. The prelude's String(const uint8*) conversion copies
 * the bytes at the call site, so the buffer only needs to stay valid until
 * the next format call. Returning the pointer (instead of a String struct)
 * keeps the ABI to a single register: the compiler returns aggregates of
 * String's size in registers, which clang's SysV sret lowering would not
 * match. */
static char *utopia_format_buffer = nullptr;

const char *utopia_string_format(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  va_list probe;
  va_copy(probe, ap);
  int len = std::vsnprintf(nullptr, 0, fmt, probe);
  va_end(probe);

  if (len < 0) {
    va_end(ap);
    return "";
  }

  size_t size = static_cast<size_t>(len);
  char *buf =
      static_cast<char *>(std::realloc(utopia_format_buffer, size + 1));
  if (!buf) {
    va_end(ap);
    return "";
  }
  utopia_format_buffer = buf;
  std::vsnprintf(buf, size + 1, fmt, ap);
  va_end(ap);
  return buf;
}

} // extern "C"
