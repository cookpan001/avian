#include "common.h"
#include "system.h"
#include "constants.h"
#include "machine.h"
#include "processor.h"
#include "process.h"

using namespace vm;

namespace {

const unsigned FrameBaseOffset = 0;
const unsigned FrameNextOffset = 1;
const unsigned FrameMethodOffset = 2;
const unsigned FrameIpOffset = 3;
const unsigned FrameFootprint = 4;

class Thread: public vm::Thread {
 public:
  static const unsigned StackSizeInBytes = 64 * 1024;
  static const unsigned StackSizeInWords = StackSizeInBytes / BytesPerWord;

  Thread(Machine* m, object javaThread, vm::Thread* parent):
    vm::Thread(m, javaThread, parent),
    ip(0),
    sp(0),
    frame(-1),
    code(0)
  { }

  unsigned ip;
  unsigned sp;
  int frame;
  object code;
  uintptr_t stack[StackSizeInWords];
};

inline void
pushObject(Thread* t, object o)
{
  if (DebugStack) {
    fprintf(stderr, "push object %p at %d\n", o, t->sp);
  }

  assert(t, t->sp + 1 < Thread::StackSizeInWords / 2);
  t->stack[(t->sp * 2)    ] = ObjectTag;
  t->stack[(t->sp * 2) + 1] = reinterpret_cast<uintptr_t>(o);
  ++ t->sp;
}

inline void
pushInt(Thread* t, uint32_t v)
{
  if (DebugStack) {
    fprintf(stderr, "push int %d at %d\n", v, t->sp);
  }

  assert(t, t->sp + 1 < Thread::StackSizeInWords / 2);
  t->stack[(t->sp * 2)    ] = IntTag;
  t->stack[(t->sp * 2) + 1] = v;
  ++ t->sp;
}

inline void
pushFloat(Thread* t, float v)
{
  pushInt(t, floatToBits(v));
}

inline void
pushLong(Thread* t, uint64_t v)
{
  if (DebugStack) {
    fprintf(stderr, "push long %"LLD" at %d\n", v, t->sp);
  }

  pushInt(t, v >> 32);
  pushInt(t, v & 0xFFFFFFFF);
}

inline void
pushDouble(Thread* t, double v)
{
  pushLong(t, doubleToBits(v));
}

inline object
popObject(Thread* t)
{
  if (DebugStack) {
    fprintf(stderr, "pop object %p at %d\n",
            reinterpret_cast<object>(t->stack[((t->sp - 1) * 2) + 1]),
            t->sp - 1);
  }

  assert(t, t->stack[(t->sp - 1) * 2] == ObjectTag);
  return reinterpret_cast<object>(t->stack[((-- t->sp) * 2) + 1]);
}

inline uint32_t
popInt(Thread* t)
{
  if (DebugStack) {
    fprintf(stderr, "pop int %"ULD" at %d\n",
            t->stack[((t->sp - 1) * 2) + 1],
            t->sp - 1);
  }

  assert(t, t->stack[(t->sp - 1) * 2] == IntTag);
  return t->stack[((-- t->sp) * 2) + 1];
}

inline float
popFloat(Thread* t)
{
  return bitsToFloat(popInt(t));
}

inline uint64_t
popLong(Thread* t)
{
  if (DebugStack) {
    fprintf(stderr, "pop long %"LLD" at %d\n",
            (static_cast<uint64_t>(t->stack[((t->sp - 2) * 2) + 1]) << 32)
            | static_cast<uint64_t>(t->stack[((t->sp - 1) * 2) + 1]),
            t->sp - 2);
  }

  uint64_t a = popInt(t);
  uint64_t b = popInt(t);
  return (b << 32) | a;
}

inline float
popDouble(Thread* t)
{
  return bitsToDouble(popLong(t));
}

inline object
peekObject(Thread* t, unsigned index)
{
  if (DebugStack) {
    fprintf(stderr, "peek object %p at %d\n",
            reinterpret_cast<object>(t->stack[(index * 2) + 1]),
            index);
  }

  assert(t, index < Thread::StackSizeInWords / 2);
  assert(t, t->stack[index * 2] == ObjectTag);
  return *reinterpret_cast<object*>(t->stack + (index * 2) + 1);
}

inline uint32_t
peekInt(Thread* t, unsigned index)
{
  if (DebugStack) {
    fprintf(stderr, "peek int %"ULD" at %d\n",
            t->stack[(index * 2) + 1],
            index);
  }

  assert(t, index < Thread::StackSizeInWords / 2);
  assert(t, t->stack[index * 2] == IntTag);
  return t->stack[(index * 2) + 1];
}

inline uint64_t
peekLong(Thread* t, unsigned index)
{
  if (DebugStack) {
    fprintf(stderr, "peek long %"LLD" at %d\n",
            (static_cast<uint64_t>(t->stack[(index * 2) + 1]) << 32)
            | static_cast<uint64_t>(t->stack[((index + 1) * 2) + 1]),
            index);
  }

  return (static_cast<uint64_t>(peekInt(t, index)) << 32)
    | static_cast<uint64_t>(peekInt(t, index + 1));
}

inline void
pokeObject(Thread* t, unsigned index, object value)
{
  if (DebugStack) {
    fprintf(stderr, "poke object %p at %d\n", value, index);
  }

  t->stack[index * 2] = ObjectTag;
  t->stack[(index * 2) + 1] = reinterpret_cast<uintptr_t>(value);
}

inline void
pokeInt(Thread* t, unsigned index, uint32_t value)
{
  if (DebugStack) {
    fprintf(stderr, "poke int %d at %d\n", value, index);
  }

  t->stack[index * 2] = IntTag;
  t->stack[(index * 2) + 1] = value;
}

inline void
pokeLong(Thread* t, unsigned index, uint64_t value)
{
  if (DebugStack) {
    fprintf(stderr, "poke long %"LLD" at %d\n", value, index);
  }

  pokeInt(t, index, value >> 32);
  pokeInt(t, index + 1, value & 0xFFFFFFFF);
}

inline object*
pushReference(Thread* t, object o)
{
  if (o) {
    expect(t, t->sp + 1 < Thread::StackSizeInWords / 2);
    pushObject(t, o);
    return reinterpret_cast<object*>(t->stack + ((t->sp - 1) * 2) + 1);
  } else {
    return 0;
  }
}

inline int
frameNext(Thread* t, int frame)
{
  return peekInt(t, frame + FrameNextOffset);
}

inline object
frameMethod(Thread* t, int frame)
{
  return peekObject(t, frame + FrameMethodOffset);
}

inline unsigned
frameIp(Thread* t, int frame)
{
  return peekInt(t, frame + FrameIpOffset);
}

inline unsigned
frameBase(Thread* t, int frame)
{
  return peekInt(t, frame + FrameBaseOffset);
}

inline object
localObject(Thread* t, unsigned index)
{
  return peekObject(t, frameBase(t, t->frame) + index);
}

inline uint32_t
localInt(Thread* t, unsigned index)
{
  return peekInt(t, frameBase(t, t->frame) + index);
}

inline uint64_t
localLong(Thread* t, unsigned index)
{
  return peekLong(t, frameBase(t, t->frame) + index);
}

inline void
setLocalObject(Thread* t, unsigned index, object value)
{
  pokeObject(t, frameBase(t, t->frame) + index, value);
}

inline void
setLocalInt(Thread* t, unsigned index, uint32_t value)
{
  pokeInt(t, frameBase(t, t->frame) + index, value);
}

inline void
setLocalLong(Thread* t, unsigned index, uint64_t value)
{
  pokeLong(t, frameBase(t, t->frame) + index, value);
}

void
pushFrame(Thread* t, object method)
{
  if (t->frame >= 0) {
    pokeInt(t, t->frame + FrameIpOffset, t->ip);
  }
  t->ip = 0;

  unsigned parameterFootprint = methodParameterFootprint(t, method);
  unsigned base = t->sp - parameterFootprint;
  unsigned locals = parameterFootprint;

  if ((methodFlags(t, method) & ACC_NATIVE) == 0) {
    t->code = methodCode(t, method);

    locals = codeMaxLocals(t, t->code);

    memset(t->stack + ((base + parameterFootprint) * 2), 0,
           (locals - parameterFootprint) * BytesPerWord * 2);
  }

  unsigned frame = base + locals;
  pokeInt(t, frame + FrameNextOffset, t->frame);
  t->frame = frame;

  t->sp = frame + FrameFootprint;

  pokeInt(t, frame + FrameBaseOffset, base);
  pokeObject(t, frame + FrameMethodOffset, method);
  pokeInt(t, t->frame + FrameIpOffset, 0);

  if (methodFlags(t, method) & ACC_SYNCHRONIZED) {
    if (methodFlags(t, method) & ACC_STATIC) {
      acquire(t, methodClass(t, method));
    } else {
      acquire(t, peekObject(t, base));
    }   
  }
}

void
popFrame(Thread* t)
{
  object method = frameMethod(t, t->frame);

  if (methodFlags(t, method) & ACC_SYNCHRONIZED) {
    if (methodFlags(t, method) & ACC_STATIC) {
      release(t, methodClass(t, method));
    } else {
      release(t, peekObject(t, frameBase(t, t->frame)));
    }   
  }
  
  if (UNLIKELY(methodVmFlags(t, method) & ClassInitFlag)) {
    if (t->exception) {
      t->exception = makeExceptionInInitializerError(t, t->exception);
    }
    classVmFlags(t, methodClass(t, method)) &= ~(NeedInitFlag | InitFlag);
    release(t, t->m->classLock);
  }

  t->sp = frameBase(t, t->frame);
  t->frame = frameNext(t, t->frame);
  if (t->frame >= 0) {
    t->code = methodCode(t, frameMethod(t, t->frame));
    t->ip = frameIp(t, t->frame);
  } else {
    t->code = 0;
    t->ip = 0;
  }
}

object
makeNativeMethodData(Thread* t, object method, void* function)
{
  PROTECT(t, method);

  unsigned count = methodParameterCount(t, method) + 2;

  object data = makeNativeMethodData(t,
                                     function,
                                     0, // argument table size
                                     count,
                                     false);
        
  unsigned argumentTableSize = BytesPerWord * 2;
  unsigned index = 0;

  nativeMethodDataParameterTypes(t, data, index++) = POINTER_TYPE;
  nativeMethodDataParameterTypes(t, data, index++) = POINTER_TYPE;

  const char* s = reinterpret_cast<const char*>
    (&byteArrayBody(t, methodSpec(t, method), 0));
  ++ s; // skip '('
  while (*s and *s != ')') {
    unsigned code = fieldCode(t, *s);
    nativeMethodDataParameterTypes(t, data, index++) = fieldType(t, code);

    switch (*s) {
    case 'L':
      argumentTableSize += BytesPerWord;
      while (*s and *s != ';') ++ s;
      ++ s;
      break;

    case '[':
      argumentTableSize += BytesPerWord;
      while (*s == '[') ++ s;
      switch (*s) {
      case 'L':
        while (*s and *s != ';') ++ s;
        ++ s;
        break;

      default:
        ++ s;
        break;
      }
      break;
      
    default:
      argumentTableSize += pad(primitiveSize(t, code));
      ++ s;
      break;
    }
  }

  nativeMethodDataArgumentTableSize(t, data) = argumentTableSize;

  return data;
}

inline object
resolveNativeMethodData(Thread* t, object method)
{
  if (objectClass(t, methodCode(t, method))
      == arrayBody(t, t->m->types, Machine::ByteArrayType))
  {
    void* p = resolveNativeMethod(t, method);
    if (LIKELY(p)) {
      PROTECT(t, method);
      object data = makeNativeMethodData(t, method, p);
      set(t, method, MethodCode, data);
      return data;
    } else {
      object message = makeString
        (t, "%s", &byteArrayBody(t, methodCode(t, method), 0));
      t->exception = makeUnsatisfiedLinkError(t, message);
      return 0;
    }
  } else {
    return methodCode(t, method);
  }  
}

inline void
checkStack(Thread* t, object method)
{
  if (UNLIKELY(t->sp
               + methodParameterFootprint(t, method)
               + codeMaxLocals(t, methodCode(t, method))
               + FrameFootprint
               + codeMaxStack(t, methodCode(t, method))
               > Thread::StackSizeInWords / 2))
  {
    t->exception = makeStackOverflowError(t);
  }
}

unsigned
invokeNative(Thread* t, object method)
{
  PROTECT(t, method);

  object data = resolveNativeMethodData(t, method);
  if (UNLIKELY(t->exception)) {
    return VoidField;
  }

  PROTECT(t, data);

  pushFrame(t, method);

  unsigned count = nativeMethodDataLength(t, data) - 1;

  unsigned size = nativeMethodDataArgumentTableSize(t, data);
  uintptr_t args[size / BytesPerWord];
  unsigned offset = 0;

  args[offset++] = reinterpret_cast<uintptr_t>(t);

  unsigned i = 0;
  if (methodFlags(t, method) & ACC_STATIC) {
    ++ i;
    args[offset++] = reinterpret_cast<uintptr_t>
      (pushReference(t, methodClass(t, method)));
  }

  unsigned sp = frameBase(t, t->frame);
  for (; i < count; ++i) {
    unsigned type = nativeMethodDataParameterTypes(t, data, i + 1);

    switch (type) {
    case INT8_TYPE:
    case INT16_TYPE:
    case INT32_TYPE:
    case FLOAT_TYPE:
      args[offset++] = peekInt(t, sp++);
      break;

    case INT64_TYPE:
    case DOUBLE_TYPE: {
      uint64_t v = peekLong(t, sp);
      memcpy(args + offset, &v, 8);
      offset += (8 / BytesPerWord);
      sp += 2;
    } break;

    case POINTER_TYPE: {
      object* v = reinterpret_cast<object*>(t->stack + ((sp++) * 2) + 1);
      if (*v == 0) {
        v = 0;
      }
      args[offset++] = reinterpret_cast<uintptr_t>(v);
    } break;

    default: abort(t);
    }
  }

  unsigned returnCode = methodReturnCode(t, method);
  unsigned returnType = fieldType(t, returnCode);
  void* function = nativeMethodDataFunction(t, data);
  uint8_t types[nativeMethodDataLength(t, data)];
  memcpy(&types,
         &nativeMethodDataParameterTypes(t, data, 0),
         nativeMethodDataLength(t, data));
  uint64_t result;

  if (DebugRun) {
    fprintf(stderr, "invoke native method %s.%s\n",
            &byteArrayBody(t, className(t, methodClass(t, method)), 0),
            &byteArrayBody(t, methodName(t, method), 0));
  }
  
  { ENTER(t, Thread::IdleState);

    result = t->m->system->call
      (function,
       args,
       types,
       count + 1,
       size,
       returnType);
  }

  if (DebugRun) {
    fprintf(stderr, "return from native method %s.%s\n",
            &byteArrayBody
            (t, className(t, methodClass(t, frameMethod(t, t->frame))), 0),
            &byteArrayBody
            (t, methodName(t, frameMethod(t, t->frame)), 0));
  }

  popFrame(t);

  if (UNLIKELY(t->exception)) {
    return VoidField;
  }

  switch (returnCode) {
  case ByteField:
  case BooleanField:
  case CharField:
  case ShortField:
  case FloatField:
  case IntField:
    if (DebugRun) {
      fprintf(stderr, "result: %"LLD"\n", result);
    }
    pushInt(t, result);
    break;

  case LongField:
  case DoubleField:
    if (DebugRun) {
      fprintf(stderr, "result: %"LLD"\n", result);
    }
    pushLong(t, result);
    break;

  case ObjectField:
    if (DebugRun) {
      fprintf(stderr, "result: %p at %p\n", result == 0 ? 0 :
              *reinterpret_cast<object*>(static_cast<uintptr_t>(result)),
              reinterpret_cast<object*>(static_cast<uintptr_t>(result)));
    }
    pushObject(t, result == 0 ? 0 :
               *reinterpret_cast<object*>(static_cast<uintptr_t>(result)));
    break;

  case VoidField:
    break;

  default:
    abort(t);
  }

  return returnCode;
}

bool
classInit2(Thread* t, object class_, unsigned ipOffset)
{
  PROTECT(t, class_);
  acquire(t, t->m->classLock);
  if (classVmFlags(t, class_) & NeedInitFlag
      and (classVmFlags(t, class_) & InitFlag) == 0)
  {
    classVmFlags(t, class_) |= InitFlag;
    t->code = classInitializer(t, class_);
    t->ip -= ipOffset;
    return true;
  } else {
    release(t, t->m->classLock);
    return false;
  }
}

inline bool
classInit(Thread* t, object class_, unsigned ipOffset)
{
  if (UNLIKELY(classVmFlags(t, class_) & NeedInitFlag)) {
    return classInit2(t, class_, ipOffset);
  } else {
    return false;
  }
}

inline void
store(Thread* t, unsigned index)
{
  memcpy(t->stack + ((frameBase(t, t->frame) + index) * 2),
         t->stack + ((-- t->sp) * 2),
         BytesPerWord * 2);
}

void
populateMultiArray(Thread* t, object array, int32_t* counts,
                   unsigned index, unsigned dimensions)
{
  if (index + 1 == dimensions or counts[index] == 0) {
    return;
  }

  PROTECT(t, array);

  object spec = className(t, objectClass(t, array));
  PROTECT(t, spec);

  object elementSpec = makeByteArray
    (t, byteArrayLength(t, spec) - 1, false);
  memcpy(&byteArrayBody(t, elementSpec, 0),
         &byteArrayBody(t, spec, 1),
         byteArrayLength(t, spec) - 1);

  object class_ = resolveClass(t, elementSpec);
  PROTECT(t, class_);

  for (int32_t i = 0; i < counts[index]; ++i) {
    object a = makeArray(t, counts[index + 1], true);
    setObjectClass(t, a, class_);
    set(t, array, ArrayBody + (i * BytesPerWord), a);
    
    populateMultiArray(t, a, counts, index + 1, dimensions);
  }
}

ExceptionHandler*
findExceptionHandler(Thread* t, int frame)
{
  object method = frameMethod(t, frame);
  object eht = codeExceptionHandlerTable(t, methodCode(t, method));
      
  if (eht) {
    for (unsigned i = 0; i < exceptionHandlerTableLength(t, eht); ++i) {
      ExceptionHandler* eh = exceptionHandlerTableBody(t, eht, i);

      if (frameIp(t, frame) - 1 >= exceptionHandlerStart(eh)
          and frameIp(t, frame) - 1 < exceptionHandlerEnd(eh))
      {
        object catchType = 0;
        if (exceptionHandlerCatchType(eh)) {
          object e = t->exception;
          t->exception = 0;
          PROTECT(t, e);

          PROTECT(t, eht);
          catchType = resolveClassInPool
            (t, codePool(t, t->code), exceptionHandlerCatchType(eh) - 1);

          if (catchType) {
            eh = exceptionHandlerTableBody(t, eht, i);
            t->exception = e;
          } else {
            // can't find what we're supposed to catch - move on.
            continue;
          }
        }

        if (catchType == 0 or instanceOf(t, catchType, t->exception)) {
          return eh;
        }
      }
    }
  }

  return 0;
}

object
interpret(Thread* t)
{
  const int base = t->frame;

  unsigned instruction = nop;
  unsigned& ip = t->ip;
  unsigned& sp = t->sp;
  int& frame = t->frame;
  object& code = t->code;
  object& exception = t->exception;
  uintptr_t* stack = t->stack;

  if (UNLIKELY(exception)) {
    goto throw_;
  }

  if (UNLIKELY(classInit(t, methodClass(t, frameMethod(t, frame)), 0))) {
    goto invoke;
  }

 loop:
  instruction = codeBody(t, code, ip++);

  if (DebugRun) {
    fprintf(stderr, "ip: %d; instruction: 0x%x in %s.%s ",
            ip - 1,
            instruction,
            &byteArrayBody
            (t, className(t, methodClass(t, frameMethod(t, frame))), 0),
            &byteArrayBody
            (t, methodName(t, frameMethod(t, frame)), 0));

    int line = t->m->processor->lineNumber(t, frameMethod(t, frame), ip);
    switch (line) {
    case NativeLine:
      fprintf(stderr, "(native)\n");
      break;
    case UnknownLine:
      fprintf(stderr, "(unknown line)\n");
      break;
    default:
      fprintf(stderr, "(line %d)\n", line);
    }
  }

  switch (instruction) {
  case aaload: {
    int32_t index = popInt(t);
    object array = popObject(t);

    if (LIKELY(array)) {
      if (LIKELY(index >= 0 and
                 static_cast<uintptr_t>(index) < objectArrayLength(t, array)))
      {
        pushObject(t, objectArrayBody(t, array, index));
      } else {
        object message = makeString(t, "%d not in [0,%d)", index,
                                    objectArrayLength(t, array));
        exception = makeArrayIndexOutOfBoundsException(t, message);
        goto throw_;
      }
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case aastore: {
    object value = popObject(t);
    int32_t index = popInt(t);
    object array = popObject(t);

    if (LIKELY(array)) {
      if (LIKELY(index >= 0 and
                 static_cast<uintptr_t>(index) < objectArrayLength(t, array)))
      {
        set(t, array, ArrayBody + (index * BytesPerWord), value);
      } else {
        object message = makeString(t, "%d not in [0,%d)", index,
                                    objectArrayLength(t, array));
        exception = makeArrayIndexOutOfBoundsException(t, message);
        goto throw_;
      }
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case aconst_null: {
    pushObject(t, 0);
  } goto loop;

  case aload: {
    pushObject(t, localObject(t, codeBody(t, code, ip++)));
  } goto loop;

  case aload_0: {
    pushObject(t, localObject(t, 0));
  } goto loop;

  case aload_1: {
    pushObject(t, localObject(t, 1));
  } goto loop;

  case aload_2: {
    pushObject(t, localObject(t, 2));
  } goto loop;

  case aload_3: {
    pushObject(t, localObject(t, 3));
  } goto loop;

  case anewarray: {
    int32_t count = popInt(t);

    if (LIKELY(count >= 0)) {
      uint16_t index = codeReadInt16(t, code, ip);
      
      object class_ = resolveClassInPool(t, codePool(t, code), index - 1);
      if (UNLIKELY(exception)) goto throw_;
            
      pushObject(t, makeObjectArray(t, class_, count, true));
    } else {
      object message = makeString(t, "%d", count);
      exception = makeNegativeArraySizeException(t, message);
      goto throw_;
    }
  } goto loop;

  case areturn: {
    object result = popObject(t);
    if (frame > base) {
      popFrame(t);
      pushObject(t, result);
      goto loop;
    } else {
      return result;
    }
  } goto loop;

  case arraylength: {
    object array = popObject(t);
    if (LIKELY(array)) {
      pushInt(t, cast<uintptr_t>(array, BytesPerWord));
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case astore: {
    store(t, codeBody(t, code, ip++));
  } goto loop;

  case astore_0: {
    store(t, 0);
  } goto loop;

  case astore_1: {
    store(t, 1);
  } goto loop;

  case astore_2: {
    store(t, 2);
  } goto loop;

  case astore_3: {
    store(t, 3);
  } goto loop;

  case athrow: {
    exception = popObject(t);
    if (UNLIKELY(exception == 0)) {
      exception = makeNullPointerException(t);      
    }
  } goto throw_;

  case baload: {
    int32_t index = popInt(t);
    object array = popObject(t);

    if (LIKELY(array)) {
      if (objectClass(t, array)
          == arrayBody(t, t->m->types, Machine::BooleanArrayType))
      {
        if (LIKELY(index >= 0 and
                   static_cast<uintptr_t>(index)
                   < booleanArrayLength(t, array)))
        {
          pushInt(t, booleanArrayBody(t, array, index));
        } else {
          object message = makeString(t, "%d not in [0,%d)", index,
                                      booleanArrayLength(t, array));
          exception = makeArrayIndexOutOfBoundsException(t, message);
          goto throw_;
        }
      } else {
        if (LIKELY(index >= 0 and
                   static_cast<uintptr_t>(index)
                   < byteArrayLength(t, array)))
        {
          pushInt(t, byteArrayBody(t, array, index));
        } else {
          object message = makeString(t, "%d not in [0,%d)", index,
                                      byteArrayLength(t, array));
          exception = makeArrayIndexOutOfBoundsException(t, message);
          goto throw_;
        }
      }
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case bastore: {
    int8_t value = popInt(t);
    int32_t index = popInt(t);
    object array = popObject(t);

    if (LIKELY(array)) {
      if (objectClass(t, array)
          == arrayBody(t, t->m->types, Machine::BooleanArrayType))
      {
        if (LIKELY(index >= 0 and
                   static_cast<uintptr_t>(index)
                   < booleanArrayLength(t, array)))
        {
          booleanArrayBody(t, array, index) = value;
        } else {
          object message = makeString(t, "%d not in [0,%d)", index,
                                      booleanArrayLength(t, array));
          exception = makeArrayIndexOutOfBoundsException(t, message);
          goto throw_;
        }
      } else {
        if (LIKELY(index >= 0 and
                   static_cast<uintptr_t>(index) < byteArrayLength(t, array)))
        {
          byteArrayBody(t, array, index) = value;
        } else {
          object message = makeString(t, "%d not in [0,%d)", index,
                                      byteArrayLength(t, array));
          exception = makeArrayIndexOutOfBoundsException(t, message);
          goto throw_;
        }
      }
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case bipush: {
    pushInt(t, static_cast<int8_t>(codeBody(t, code, ip++)));
  } goto loop;

  case caload: {
    int32_t index = popInt(t);
    object array = popObject(t);

    if (LIKELY(array)) {
      if (LIKELY(index >= 0 and
                 static_cast<uintptr_t>(index) < charArrayLength(t, array)))
      {
        pushInt(t, charArrayBody(t, array, index));
      } else {
        object message = makeString(t, "%d not in [0,%d)", index,
                                    charArrayLength(t, array));
        exception = makeArrayIndexOutOfBoundsException(t, message);
        goto throw_;
      }
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case castore: {
    uint16_t value = popInt(t);
    int32_t index = popInt(t);
    object array = popObject(t);

    if (LIKELY(array)) {
      if (LIKELY(index >= 0 and
                 static_cast<uintptr_t>(index) < charArrayLength(t, array)))
      {
        charArrayBody(t, array, index) = value;
      } else {
        object message = makeString(t, "%d not in [0,%d)", index,
                                    charArrayLength(t, array));
        exception = makeArrayIndexOutOfBoundsException(t, message);
        goto throw_;
      }
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case checkcast: {
    uint16_t index = codeReadInt16(t, code, ip);

    if (peekObject(t, sp - 1)) {
      object class_ = resolveClassInPool(t, codePool(t, code), index - 1);
      if (UNLIKELY(exception)) goto throw_;

      if (not instanceOf(t, class_, peekObject(t, sp - 1))) {
        object message = makeString
          (t, "%s as %s",
           &byteArrayBody
           (t, className(t, objectClass(t, peekObject(t, sp - 1))), 0),
           &byteArrayBody(t, className(t, class_), 0));
        exception = makeClassCastException(t, message);
        goto throw_;
      }
    }
  } goto loop;

  case d2f: {
    pushFloat(t, static_cast<float>(popDouble(t)));
  } goto loop;

  case d2i: {
    pushInt(t, static_cast<int32_t>(popDouble(t)));
  } goto loop;

  case d2l: {
    pushLong(t, static_cast<int64_t>(popDouble(t)));
  } goto loop;

  case dadd: {
    double b = popDouble(t);
    double a = popDouble(t);
    
    pushDouble(t, a + b);
  } goto loop;

  case daload: {
    int32_t index = popInt(t);
    object array = popObject(t);

    if (LIKELY(array)) {
      if (LIKELY(index >= 0 and
                 static_cast<uintptr_t>(index) < doubleArrayLength(t, array)))
      {
        pushLong(t, doubleArrayBody(t, array, index));
      } else {
        object message = makeString(t, "%d not in [0,%d)", index,
                                    doubleArrayLength(t, array));
        exception = makeArrayIndexOutOfBoundsException(t, message);
        goto throw_;
      }
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case dastore: {
    double value = popDouble(t);
    int32_t index = popInt(t);
    object array = popObject(t);

    if (LIKELY(array)) {
      if (LIKELY(index >= 0 and
                 static_cast<uintptr_t>(index) < doubleArrayLength(t, array)))
      {
        memcpy(&doubleArrayBody(t, array, index), &value, sizeof(uint64_t));
      } else {
        object message = makeString(t, "%d not in [0,%d)", index,
                                    doubleArrayLength(t, array));
        exception = makeArrayIndexOutOfBoundsException(t, message);
        goto throw_;
      }
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case dcmpg: {
    double b = popDouble(t);
    double a = popDouble(t);
    
    if (a < b) {
      pushInt(t, static_cast<unsigned>(-1));
    } else if (a > b) {
      pushInt(t, 1);
    } else if (a == b) {
      pushInt(t, 0);
    } else {
      pushInt(t, 1);
    }
  } goto loop;

  case dcmpl: {
    double b = popDouble(t);
    double a = popDouble(t);
    
    if (a < b) {
      pushInt(t, static_cast<unsigned>(-1));
    } else if (a > b) {
      pushInt(t, 1);
    } else if (a == b) {
      pushInt(t, 0);
    } else {
      pushInt(t, static_cast<unsigned>(-1));
    }
  } goto loop;

  case dconst_0: {
    pushDouble(t, 0);
  } goto loop;

  case dconst_1: {
    pushDouble(t, 1);
  } goto loop;

  case ddiv: {
    double b = popDouble(t);
    double a = popDouble(t);
    
    pushDouble(t, a / b);
  } goto loop;

  case dmul: {
    double b = popDouble(t);
    double a = popDouble(t);
    
    pushDouble(t, a * b);
  } goto loop;

  case dneg: {
    double a = popDouble(t);
    
    pushDouble(t, - a);
  } goto loop;

  case vm::drem: {
    double b = popDouble(t);
    double a = popDouble(t);
    
    pushDouble(t, fmod(a, b));
  } goto loop;

  case dsub: {
    double b = popDouble(t);
    double a = popDouble(t);
    
    pushDouble(t, a - b);
  } goto loop;

  case dup: {
    if (DebugStack) {
      fprintf(stderr, "dup\n");
    }

    memcpy(stack + ((sp    ) * 2), stack + ((sp - 1) * 2), BytesPerWord * 2);
    ++ sp;
  } goto loop;

  case dup_x1: {
    if (DebugStack) {
      fprintf(stderr, "dup_x1\n");
    }

    memcpy(stack + ((sp    ) * 2), stack + ((sp - 1) * 2), BytesPerWord * 2);
    memcpy(stack + ((sp - 1) * 2), stack + ((sp - 2) * 2), BytesPerWord * 2);
    memcpy(stack + ((sp - 2) * 2), stack + ((sp    ) * 2), BytesPerWord * 2);
    ++ sp;
  } goto loop;

  case dup_x2: {
    if (DebugStack) {
      fprintf(stderr, "dup_x2\n");
    }

    memcpy(stack + ((sp    ) * 2), stack + ((sp - 1) * 2), BytesPerWord * 2);
    memcpy(stack + ((sp - 1) * 2), stack + ((sp - 2) * 2), BytesPerWord * 2);
    memcpy(stack + ((sp - 2) * 2), stack + ((sp - 3) * 2), BytesPerWord * 2);
    memcpy(stack + ((sp - 3) * 2), stack + ((sp    ) * 2), BytesPerWord * 2);
    ++ sp;
  } goto loop;

  case dup2: {
    if (DebugStack) {
      fprintf(stderr, "dup2\n");
    }

    memcpy(stack + ((sp    ) * 2), stack + ((sp - 2) * 2), BytesPerWord * 4);
    sp += 2;
  } goto loop;

  case dup2_x1: {
    if (DebugStack) {
      fprintf(stderr, "dup2_x1\n");
    }

    memcpy(stack + ((sp + 1) * 2), stack + ((sp - 1) * 2), BytesPerWord * 2);
    memcpy(stack + ((sp    ) * 2), stack + ((sp - 2) * 2), BytesPerWord * 2);
    memcpy(stack + ((sp - 1) * 2), stack + ((sp - 3) * 2), BytesPerWord * 2);
    memcpy(stack + ((sp - 3) * 2), stack + ((sp    ) * 2), BytesPerWord * 4);
    sp += 2;
  } goto loop;

  case dup2_x2: {
    if (DebugStack) {
      fprintf(stderr, "dup2_x2\n");
    }

    memcpy(stack + ((sp + 1) * 2), stack + ((sp - 1) * 2), BytesPerWord * 2);
    memcpy(stack + ((sp    ) * 2), stack + ((sp - 2) * 2), BytesPerWord * 2);
    memcpy(stack + ((sp - 1) * 2), stack + ((sp - 3) * 2), BytesPerWord * 2);
    memcpy(stack + ((sp - 2) * 2), stack + ((sp - 4) * 2), BytesPerWord * 2);
    memcpy(stack + ((sp - 4) * 2), stack + ((sp    ) * 2), BytesPerWord * 4);
    sp += 2;
  } goto loop;

  case f2d: {
    pushDouble(t, popFloat(t));
  } goto loop;

  case f2i: {
    pushInt(t, static_cast<int32_t>(popFloat(t)));
  } goto loop;

  case f2l: {
    pushLong(t, static_cast<int64_t>(popFloat(t)));
  } goto loop;

  case fadd: {
    float b = popFloat(t);
    float a = popFloat(t);
    
    pushFloat(t, a + b);
  } goto loop;

  case faload: {
    int32_t index = popInt(t);
    object array = popObject(t);

    if (LIKELY(array)) {
      if (LIKELY(index >= 0 and
                 static_cast<uintptr_t>(index) < floatArrayLength(t, array)))
      {
        pushInt(t, floatArrayBody(t, array, index));
      } else {
        object message = makeString(t, "%d not in [0,%d)", index,
                                    floatArrayLength(t, array));
        exception = makeArrayIndexOutOfBoundsException(t, message);
        goto throw_;
      }
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case fastore: {
    float value = popFloat(t);
    int32_t index = popInt(t);
    object array = popObject(t);

    if (LIKELY(array)) {
      if (LIKELY(index >= 0 and
                 static_cast<uintptr_t>(index) < floatArrayLength(t, array)))
      {
        memcpy(&floatArrayBody(t, array, index), &value, sizeof(uint32_t));
      } else {
        object message = makeString(t, "%d not in [0,%d)", index,
                                    floatArrayLength(t, array));
        exception = makeArrayIndexOutOfBoundsException(t, message);
        goto throw_;
      }
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case fcmpg: {
    float b = popFloat(t);
    float a = popFloat(t);
    
    if (a < b) {
      pushInt(t, static_cast<unsigned>(-1));
    } else if (a > b) {
      pushInt(t, 1);
    } else if (a == b) {
      pushInt(t, 0);
    } else {
      pushInt(t, 1);
    }
  } goto loop;

  case fcmpl: {
    float b = popFloat(t);
    float a = popFloat(t);
    
    if (a < b) {
      pushInt(t, static_cast<unsigned>(-1));
    } else if (a > b) {
      pushInt(t, 1);
    } else if (a == b) {
      pushInt(t, 0);
    } else {
      pushInt(t, static_cast<unsigned>(-1));
    }
  } goto loop;

  case fconst_0: {
    pushFloat(t, 0);
  } goto loop;

  case fconst_1: {
    pushFloat(t, 1);
  } goto loop;

  case fconst_2: {
    pushFloat(t, 2);
  } goto loop;

  case fdiv: {
    float b = popFloat(t);
    float a = popFloat(t);
    
    pushFloat(t, a / b);
  } goto loop;

  case fmul: {
    float b = popFloat(t);
    float a = popFloat(t);
    
    pushFloat(t, a * b);
  } goto loop;

  case fneg: {
    float a = popFloat(t);
    
    pushFloat(t, - a);
  } goto loop;

  case frem: {
    float b = popFloat(t);
    float a = popFloat(t);
    
    pushFloat(t, fmodf(a, b));
  } goto loop;

  case fsub: {
    float b = popFloat(t);
    float a = popFloat(t);
    
    pushFloat(t, a - b);
  } goto loop;

  case getfield: {
    if (LIKELY(peekObject(t, sp - 1))) {
      uint16_t index = codeReadInt16(t, code, ip);
    
      object field = resolveField(t, codePool(t, code), index - 1);
      if (UNLIKELY(exception)) goto throw_;
      
      object instance = popObject(t);

      switch (fieldCode(t, field)) {
      case ByteField:
      case BooleanField:
        pushInt(t, cast<int8_t>(instance, fieldOffset(t, field)));
        break;

      case CharField:
      case ShortField:
        pushInt(t, cast<int16_t>(instance, fieldOffset(t, field)));
        break;

      case FloatField:
      case IntField:
        pushInt(t, cast<int32_t>(instance, fieldOffset(t, field)));
        break;

      case DoubleField:
      case LongField:
        pushLong(t, cast<int64_t>(instance, fieldOffset(t, field)));
        break;

      case ObjectField:
        pushObject(t, cast<object>(instance, fieldOffset(t, field)));
        break;

      default:
        abort(t);
      }
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case getstatic: {
    uint16_t index = codeReadInt16(t, code, ip);

    object field = resolveField(t, codePool(t, code), index - 1);
    if (UNLIKELY(exception)) goto throw_;
    PROTECT(t, field);

    if (UNLIKELY(classInit(t, fieldClass(t, field), 3))) goto invoke;

    object v = arrayBody(t, classStaticTable(t, fieldClass(t, field)),
                         fieldOffset(t, field));

    switch (fieldCode(t, field)) {
    case ByteField:
    case BooleanField:
    case CharField:
    case ShortField:
    case FloatField:
    case IntField:
      pushInt(t, v ? intValue(t, v) : 0);
      break;

    case DoubleField:
    case LongField:
      pushLong(t, v ? longValue(t, v) : 0);
      break;

    case ObjectField:
      pushObject(t, v);
      break;

    default: abort(t);
    }
  } goto loop;

  case goto_: {
    int16_t offset = codeReadInt16(t, code, ip);
    ip = (ip - 3) + offset;
  } goto loop;
    
  case goto_w: {
    int32_t offset = codeReadInt32(t, code, ip);
    ip = (ip - 5) + offset;
  } goto loop;

  case i2b: {
    pushInt(t, static_cast<int8_t>(popInt(t)));
  } goto loop;

  case i2c: {
    pushInt(t, static_cast<uint16_t>(popInt(t)));
  } goto loop;

  case i2d: {
    pushDouble(t, static_cast<double>(popInt(t)));
  } goto loop;

  case i2f: {
    pushFloat(t, static_cast<float>(popInt(t)));
  } goto loop;

  case i2l: {
    pushLong(t, static_cast<int32_t>(popInt(t)));
  } goto loop;

  case i2s: {
    pushInt(t, static_cast<int16_t>(popInt(t)));
  } goto loop;

  case iadd: {
    int32_t b = popInt(t);
    int32_t a = popInt(t);
    
    pushInt(t, a + b);
  } goto loop;

  case iaload: {
    int32_t index = popInt(t);
    object array = popObject(t);

    if (LIKELY(array)) {
      if (LIKELY(index >= 0 and
                 static_cast<uintptr_t>(index) < intArrayLength(t, array)))
      {
        pushInt(t, intArrayBody(t, array, index));
      } else {
        object message = makeString(t, "%d not in [0,%d)", index,
                                    intArrayLength(t, array));
        exception = makeArrayIndexOutOfBoundsException(t, message);
        goto throw_;
      }
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case iand: {
    int32_t b = popInt(t);
    int32_t a = popInt(t);
    
    pushInt(t, a & b);
  } goto loop;

  case iastore: {
    int32_t value = popInt(t);
    int32_t index = popInt(t);
    object array = popObject(t);

    if (LIKELY(array)) {
      if (LIKELY(index >= 0 and
                 static_cast<uintptr_t>(index) < intArrayLength(t, array)))
      {
        intArrayBody(t, array, index) = value;
      } else {
        object message = makeString(t, "%d not in [0,%d)", index,
                                    intArrayLength(t, array));
        exception = makeArrayIndexOutOfBoundsException(t, message);
        goto throw_;
      }
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case iconst_m1: {
    pushInt(t, static_cast<unsigned>(-1));
  } goto loop;

  case iconst_0: {
    pushInt(t, 0);
  } goto loop;

  case iconst_1: {
    pushInt(t, 1);
  } goto loop;

  case iconst_2: {
    pushInt(t, 2);
  } goto loop;

  case iconst_3: {
    pushInt(t, 3);
  } goto loop;

  case iconst_4: {
    pushInt(t, 4);
  } goto loop;

  case iconst_5: {
    pushInt(t, 5);
  } goto loop;

  case idiv: {
    int32_t b = popInt(t);
    int32_t a = popInt(t);
    
    pushInt(t, a / b);
  } goto loop;

  case if_acmpeq: {
    int16_t offset = codeReadInt16(t, code, ip);

    object b = popObject(t);
    object a = popObject(t);
    
    if (a == b) {
      ip = (ip - 3) + offset;
    }
  } goto loop;

  case if_acmpne: {
    int16_t offset = codeReadInt16(t, code, ip);

    object b = popObject(t);
    object a = popObject(t);
    
    if (a != b) {
      ip = (ip - 3) + offset;
    }
  } goto loop;

  case if_icmpeq: {
    int16_t offset = codeReadInt16(t, code, ip);

    int32_t b = popInt(t);
    int32_t a = popInt(t);
    
    if (a == b) {
      ip = (ip - 3) + offset;
    }
  } goto loop;

  case if_icmpne: {
    int16_t offset = codeReadInt16(t, code, ip);

    int32_t b = popInt(t);
    int32_t a = popInt(t);
    
    if (a != b) {
      ip = (ip - 3) + offset;
    }
  } goto loop;

  case if_icmpgt: {
    int16_t offset = codeReadInt16(t, code, ip);

    int32_t b = popInt(t);
    int32_t a = popInt(t);
    
    if (a > b) {
      ip = (ip - 3) + offset;
    }
  } goto loop;

  case if_icmpge: {
    int16_t offset = codeReadInt16(t, code, ip);

    int32_t b = popInt(t);
    int32_t a = popInt(t);
    
    if (a >= b) {
      ip = (ip - 3) + offset;
    }
  } goto loop;

  case if_icmplt: {
    int16_t offset = codeReadInt16(t, code, ip);

    int32_t b = popInt(t);
    int32_t a = popInt(t);
    
    if (a < b) {
      ip = (ip - 3) + offset;
    }
  } goto loop;

  case if_icmple: {
    int16_t offset = codeReadInt16(t, code, ip);

    int32_t b = popInt(t);
    int32_t a = popInt(t);
    
    if (a <= b) {
      ip = (ip - 3) + offset;
    }
  } goto loop;

  case ifeq: {
    int16_t offset = codeReadInt16(t, code, ip);

    if (popInt(t) == 0) {
      ip = (ip - 3) + offset;
    }
  } goto loop;

  case ifne: {
    int16_t offset = codeReadInt16(t, code, ip);

    if (popInt(t)) {
      ip = (ip - 3) + offset;
    }
  } goto loop;

  case ifgt: {
    int16_t offset = codeReadInt16(t, code, ip);

    if (static_cast<int32_t>(popInt(t)) > 0) {
      ip = (ip - 3) + offset;
    }
  } goto loop;

  case ifge: {
    int16_t offset = codeReadInt16(t, code, ip);

    if (static_cast<int32_t>(popInt(t)) >= 0) {
      ip = (ip - 3) + offset;
    }
  } goto loop;

  case iflt: {
    int16_t offset = codeReadInt16(t, code, ip);

    if (static_cast<int32_t>(popInt(t)) < 0) {
      ip = (ip - 3) + offset;
    }
  } goto loop;

  case ifle: {
    int16_t offset = codeReadInt16(t, code, ip);

    if (static_cast<int32_t>(popInt(t)) <= 0) {
      ip = (ip - 3) + offset;
    }
  } goto loop;

  case ifnonnull: {
    int16_t offset = codeReadInt16(t, code, ip);

    if (popObject(t)) {
      ip = (ip - 3) + offset;
    }
  } goto loop;

  case ifnull: {
    int16_t offset = codeReadInt16(t, code, ip);

    if (popObject(t) == 0) {
      ip = (ip - 3) + offset;
    }
  } goto loop;

  case iinc: {
    uint8_t index = codeBody(t, code, ip++);
    int8_t c = codeBody(t, code, ip++);
    
    setLocalInt(t, index, localInt(t, index) + c);
  } goto loop;

  case iload:
  case fload: {
    pushInt(t, localInt(t, codeBody(t, code, ip++)));
  } goto loop;

  case iload_0:
  case fload_0: {
    pushInt(t, localInt(t, 0));
  } goto loop;

  case iload_1:
  case fload_1: {
    pushInt(t, localInt(t, 1));
  } goto loop;

  case iload_2:
  case fload_2: {
    pushInt(t, localInt(t, 2));
  } goto loop;

  case iload_3:
  case fload_3: {
    pushInt(t, localInt(t, 3));
  } goto loop;

  case imul: {
    int32_t b = popInt(t);
    int32_t a = popInt(t);
    
    pushInt(t, a * b);
  } goto loop;

  case ineg: {
    pushInt(t, - popInt(t));
  } goto loop;

  case instanceof: {
    uint16_t index = codeReadInt16(t, code, ip);

    if (peekObject(t, sp - 1)) {
      object class_ = resolveClassInPool(t, codePool(t, code), index - 1);
      if (UNLIKELY(exception)) goto throw_;

      if (instanceOf(t, class_, popObject(t))) {
        pushInt(t, 1);
      } else {
        pushInt(t, 0);
      }
    } else {
      popObject(t);
      pushInt(t, 0);
    }
  } goto loop;

  case invokeinterface: {
    uint16_t index = codeReadInt16(t, code, ip);
    
    ip += 2;

    object method = resolveMethod(t, codePool(t, code), index - 1);
    if (UNLIKELY(exception)) goto throw_;
    
    unsigned parameterFootprint = methodParameterFootprint(t, method);
    if (LIKELY(peekObject(t, sp - parameterFootprint))) {
      code = findInterfaceMethod
        (t, method, objectClass(t, peekObject(t, sp - parameterFootprint)));
      goto invoke;
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case invokespecial: {
    uint16_t index = codeReadInt16(t, code, ip);

    object method = resolveMethod(t, codePool(t, code), index - 1);
    if (UNLIKELY(exception)) goto throw_;
    
    unsigned parameterFootprint = methodParameterFootprint(t, method);
    if (LIKELY(peekObject(t, sp - parameterFootprint))) {
      object class_ = methodClass(t, frameMethod(t, frame));
      if (isSpecialMethod(t, method, class_)) {
        class_ = classSuper(t, class_);
        if (UNLIKELY(classInit(t, class_, 3))) goto invoke;

        code = findMethod(t, method, class_);
      } else {
        code = method;
      }
      
      goto invoke;
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case invokestatic: {
    uint16_t index = codeReadInt16(t, code, ip);

    object method = resolveMethod(t, codePool(t, code), index - 1);
    if (UNLIKELY(exception)) goto throw_;
    PROTECT(t, method);
    
    if (UNLIKELY(classInit(t, methodClass(t, method), 3))) goto invoke;

    code = method;
  } goto invoke;

  case invokevirtual: {
    uint16_t index = codeReadInt16(t, code, ip);

    object method = resolveMethod(t, codePool(t, code), index - 1);
    if (UNLIKELY(exception)) goto throw_;
    
    unsigned parameterFootprint = methodParameterFootprint(t, method);
    if (LIKELY(peekObject(t, sp - parameterFootprint))) {
      object class_ = objectClass(t, peekObject(t, sp - parameterFootprint));
      if (UNLIKELY(classInit(t, class_, 3))) goto invoke;

      code = findMethod(t, method, class_);
      goto invoke;
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case ior: {
    int32_t b = popInt(t);
    int32_t a = popInt(t);
    
    pushInt(t, a | b);
  } goto loop;

  case irem: {
    int32_t b = popInt(t);
    int32_t a = popInt(t);
    
    pushInt(t, a % b);
  } goto loop;

  case ireturn:
  case freturn: {
    int32_t result = popInt(t);
    if (frame > base) {
      popFrame(t);
      pushInt(t, result);
      goto loop;
    } else {
      return makeInt(t, result);
    }
  } goto loop;

  case ishl: {
    int32_t b = popInt(t);
    int32_t a = popInt(t);
    
    pushInt(t, a << b);
  } goto loop;

  case ishr: {
    int32_t b = popInt(t);
    int32_t a = popInt(t);
    
    pushInt(t, a >> b);
  } goto loop;

  case istore:
  case fstore: {
    setLocalInt(t, codeBody(t, code, ip++), popInt(t));
  } goto loop;

  case istore_0:
  case fstore_0: {
    setLocalInt(t, 0, popInt(t));
  } goto loop;

  case istore_1:
  case fstore_1: {
    setLocalInt(t, 1, popInt(t));
  } goto loop;

  case istore_2:
  case fstore_2: {
    setLocalInt(t, 2, popInt(t));
  } goto loop;

  case istore_3:
  case fstore_3: {
    setLocalInt(t, 3, popInt(t));
  } goto loop;

  case isub: {
    int32_t b = popInt(t);
    int32_t a = popInt(t);
    
    pushInt(t, a - b);
  } goto loop;

  case iushr: {
    int32_t b = popInt(t);
    uint32_t a = popInt(t);
    
    pushInt(t, a >> b);
  } goto loop;

  case ixor: {
    int32_t b = popInt(t);
    int32_t a = popInt(t);
    
    pushInt(t, a ^ b);
  } goto loop;

  case jsr: {
    uint16_t offset = codeReadInt16(t, code, ip);

    pushInt(t, ip);
    ip = (ip - 3) + static_cast<int16_t>(offset);
  } goto loop;

  case jsr_w: {
    uint32_t offset = codeReadInt32(t, code, ip);

    pushInt(t, ip);
    ip = (ip - 3) + static_cast<int32_t>(offset);
  } goto loop;

  case l2i: {
    pushInt(t, static_cast<int32_t>(popLong(t)));
  } goto loop;

  case ladd: {
    int64_t b = popLong(t);
    int64_t a = popLong(t);
    
    pushLong(t, a + b);
  } goto loop;

  case laload: {
    int32_t index = popInt(t);
    object array = popObject(t);

    if (LIKELY(array)) {
      if (LIKELY(index >= 0 and
                 static_cast<uintptr_t>(index) < longArrayLength(t, array)))
      {
        pushLong(t, longArrayBody(t, array, index));
      } else {
        object message = makeString(t, "%d not in [0,%d)", index,
                                    longArrayLength(t, array));
        exception = makeArrayIndexOutOfBoundsException(t, message);
        goto throw_;
      }
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case land: {
    int64_t b = popLong(t);
    int64_t a = popLong(t);
    
    pushLong(t, a & b);
  } goto loop;

  case lastore: {
    int64_t value = popLong(t);
    int32_t index = popInt(t);
    object array = popObject(t);

    if (LIKELY(array)) {
      if (LIKELY(index >= 0 and
                 static_cast<uintptr_t>(index) < longArrayLength(t, array)))
      {
        longArrayBody(t, array, index) = value;
      } else {
        object message = makeString(t, "%d not in [0,%d)", index,
                                    longArrayLength(t, array));
        exception = makeArrayIndexOutOfBoundsException(t, message);
        goto throw_;
      }
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case lcmp: {
    int64_t b = popLong(t);
    int64_t a = popLong(t);
    
    pushInt(t, a > b ? 1 : a == b ? 0 : -1);
  } goto loop;

  case lconst_0: {
    pushLong(t, 0);
  } goto loop;

  case lconst_1: {
    pushLong(t, 1);
  } goto loop;

  case ldc:
  case ldc_w: {
    uint16_t index;

    if (instruction == ldc) {
      index = codeBody(t, code, ip++);
    } else {
      index = codeReadInt16(t, code, ip);
    }

    object v = arrayBody(t, codePool(t, code), index - 1);

    if (objectClass(t, v) == arrayBody(t, t->m->types, Machine::IntType)) {
      pushInt(t, intValue(t, v));
    } else if (objectClass(t, v)
               == arrayBody(t, t->m->types, Machine::FloatType))
    {
      pushInt(t, floatValue(t, v));
    } else if (objectClass(t, v)
               == arrayBody(t, t->m->types, Machine::StringType))
    {
      pushObject(t, v);
    } else {
      object class_ = resolveClassInPool(t, codePool(t, code), index - 1);
      if (UNLIKELY(exception)) goto throw_;

      pushObject(t, class_);
    }
  } goto loop;

  case ldc2_w: {
    uint16_t index = codeReadInt16(t, code, ip);

    object v = arrayBody(t, codePool(t, code), index - 1);

    if (objectClass(t, v) == arrayBody(t, t->m->types, Machine::LongType)) {
      pushLong(t, longValue(t, v));
    } else if (objectClass(t, v)
               == arrayBody(t, t->m->types, Machine::DoubleType))
    {
      pushLong(t, doubleValue(t, v));
    } else {
      abort(t);
    }
  } goto loop;

  case ldiv_: {
    int64_t b = popLong(t);
    int64_t a = popLong(t);
    
    pushLong(t, a / b);
  } goto loop;

  case lload:
  case dload: {
    pushLong(t, localLong(t, codeBody(t, code, ip++)));
  } goto loop;

  case lload_0:
  case dload_0: {
    pushLong(t, localLong(t, 0));
  } goto loop;

  case lload_1:
  case dload_1: {
    pushLong(t, localLong(t, 1));
  } goto loop;

  case lload_2:
  case dload_2: {
    pushLong(t, localLong(t, 2));
  } goto loop;

  case lload_3:
  case dload_3: {
    pushLong(t, localLong(t, 3));
  } goto loop;

  case lmul: {
    int64_t b = popLong(t);
    int64_t a = popLong(t);
    
    pushLong(t, a * b);
  } goto loop;

  case lneg: {
    pushLong(t, - popLong(t));
  } goto loop;

  case lookupswitch: {
    int32_t base = ip - 1;

    ip += 3;
    ip -= (ip % 4);
    
    int32_t default_ = codeReadInt32(t, code, ip);
    int32_t pairCount = codeReadInt32(t, code, ip);
    
    int32_t key = popInt(t);

    int32_t bottom = 0;
    int32_t top = pairCount;
    for (int32_t span = top - bottom; span; span = top - bottom) {
      int32_t middle = bottom + (span / 2);
      unsigned index = ip + (middle * 8);

      int32_t k = codeReadInt32(t, code, index);

      if (key < k) {
        top = middle;
      } else if (key > k) {
        bottom = middle + 1;
      } else {
        ip = base + codeReadInt32(t, code, index);
        goto loop;
      }
    }

    ip = base + default_;
  } goto loop;

  case lor: {
    int64_t b = popLong(t);
    int64_t a = popLong(t);
    
    pushLong(t, a | b);
  } goto loop;

  case lrem: {
    int64_t b = popLong(t);
    int64_t a = popLong(t);
    
    pushLong(t, a % b);
  } goto loop;

  case lreturn:
  case dreturn: {
    int64_t result = popLong(t);
    if (frame > base) {
      popFrame(t);
      pushLong(t, result);
      goto loop;
    } else {
      return makeLong(t, result);
    }
  } goto loop;

  case lshl: {
    int32_t b = popInt(t);
    int64_t a = popLong(t);
    
    pushLong(t, a << b);
  } goto loop;

  case lshr: {
    int32_t b = popInt(t);
    int64_t a = popLong(t);
    
    pushLong(t, a >> b);
  } goto loop;

  case lstore:
  case dstore: {
    setLocalLong(t, codeBody(t, code, ip++), popLong(t));
  } goto loop;

  case lstore_0: 
  case dstore_0:{
    setLocalLong(t, 0, popLong(t));
  } goto loop;

  case lstore_1: 
  case dstore_1: {
    setLocalLong(t, 1, popLong(t));
  } goto loop;

  case lstore_2: 
  case dstore_2: {
    setLocalLong(t, 2, popLong(t));
  } goto loop;

  case lstore_3: 
  case dstore_3: {
    setLocalLong(t, 3, popLong(t));
  } goto loop;

  case lsub: {
    int64_t b = popLong(t);
    int64_t a = popLong(t);
    
    pushLong(t, a - b);
  } goto loop;

  case lushr: {
    int64_t b = popLong(t);
    uint64_t a = popLong(t);
    
    pushLong(t, a >> b);
  } goto loop;

  case lxor: {
    int64_t b = popLong(t);
    int64_t a = popLong(t);
    
    pushLong(t, a ^ b);
  } goto loop;

  case monitorenter: {
    object o = popObject(t);
    if (LIKELY(o)) {
      acquire(t, o);
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case monitorexit: {
    object o = popObject(t);
    if (LIKELY(o)) {
      release(t, o);
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case multianewarray: {
    uint16_t index = codeReadInt16(t, code, ip);
    uint8_t dimensions = codeBody(t, code, ip++);

    object class_ = resolveClassInPool(t, codePool(t, code), index - 1);
    if (UNLIKELY(exception)) goto throw_;
    PROTECT(t, class_);

    int32_t counts[dimensions];
    for (int i = dimensions - 1; i >= 0; --i) {
      counts[i] = popInt(t);
      if (UNLIKELY(counts[i] < 0)) {
        object message = makeString(t, "%d", counts[i]);
        exception = makeNegativeArraySizeException(t, message);
        goto throw_;
      }
    }

    object array = makeArray(t, counts[0], true);
    setObjectClass(t, array, class_);
    PROTECT(t, array);

    populateMultiArray(t, array, counts, 0, dimensions);

    pushObject(t, array);
  } goto loop;

  case new_: {
    uint16_t index = codeReadInt16(t, code, ip);
    
    object class_ = resolveClassInPool(t, codePool(t, code), index - 1);
    if (UNLIKELY(exception)) goto throw_;
    PROTECT(t, class_);

    if (UNLIKELY(classInit(t, class_, 3))) goto invoke;

    pushObject(t, make(t, class_));
  } goto loop;

  case newarray: {
    int32_t count = popInt(t);

    if (LIKELY(count >= 0)) {
      uint8_t type = codeBody(t, code, ip++);

      object array;

      switch (type) {
      case T_BOOLEAN:
        array = makeBooleanArray(t, count, true);
        break;

      case T_CHAR:
        array = makeCharArray(t, count, true);
        break;

      case T_FLOAT:
        array = makeFloatArray(t, count, true);
        break;

      case T_DOUBLE:
        array = makeDoubleArray(t, count, true);
        break;

      case T_BYTE:
        array = makeByteArray(t, count, true);
        break;

      case T_SHORT:
        array = makeShortArray(t, count, true);
        break;

      case T_INT:
        array = makeIntArray(t, count, true);
        break;

      case T_LONG:
        array = makeLongArray(t, count, true);
        break;

      default: abort(t);
      }
            
      pushObject(t, array);
    } else {
      object message = makeString(t, "%d", count);
      exception = makeNegativeArraySizeException(t, message);
      goto throw_;
    }
  } goto loop;

  case nop: goto loop;

  case pop_: {
    -- sp;
  } goto loop;

  case pop2: {
    sp -= 2;
  } goto loop;

  case putfield: {
    uint16_t index = codeReadInt16(t, code, ip);
    
    object field = resolveField(t, codePool(t, code), index - 1);
    if (UNLIKELY(exception)) goto throw_;
      
    switch (fieldCode(t, field)) {
    case ByteField:
    case BooleanField:
    case CharField:
    case ShortField:
    case FloatField:
    case IntField: {
      int32_t value = popInt(t);
      object o = popObject(t);
      if (LIKELY(o)) {
        switch (fieldCode(t, field)) {
        case ByteField:
        case BooleanField:
          cast<int8_t>(o, fieldOffset(t, field)) = value;
          break;
            
        case CharField:
        case ShortField:
          cast<int16_t>(o, fieldOffset(t, field)) = value;
          break;
            
        case FloatField:
        case IntField:
          cast<int32_t>(o, fieldOffset(t, field)) = value;
          break;
        }
      } else {
        exception = makeNullPointerException(t);
        goto throw_;
      }
    } break;

    case DoubleField:
    case LongField: {
      int64_t value = popLong(t);
      object o = popObject(t);
      if (LIKELY(o)) {
        cast<int64_t>(o, fieldOffset(t, field)) = value;
      } else {
        exception = makeNullPointerException(t);
        goto throw_;
      }
    } break;

    case ObjectField: {
      object value = popObject(t);
      object o = popObject(t);
      if (LIKELY(o)) {
        set(t, o, fieldOffset(t, field), value);
      } else {
        exception = makeNullPointerException(t);
        goto throw_;
      }
    } break;

    default: abort(t);
    }
  } goto loop;

  case putstatic: {
    uint16_t index = codeReadInt16(t, code, ip);

    object field = resolveField(t, codePool(t, code), index - 1);
    if (UNLIKELY(exception)) goto throw_;
    PROTECT(t, field);

    if (UNLIKELY(classInit(t, fieldClass(t, field), 3))) goto invoke;
      
    object v;

    switch (fieldCode(t, field)) {
    case ByteField:
    case BooleanField:
    case CharField:
    case ShortField:
    case FloatField:
    case IntField: {
      v = makeInt(t, popInt(t));
    } break;

    case DoubleField:
    case LongField: {
      v = makeLong(t, popLong(t));
    } break;

    case ObjectField:
      v = popObject(t);
      break;

    default: abort(t);
    }

    set(t, classStaticTable(t, fieldClass(t, field)),
        ArrayBody + (fieldOffset(t, field) * BytesPerWord), v);
  } goto loop;

  case ret: {
    ip = localInt(t, codeBody(t, code, ip));
  } goto loop;

  case return_: {
    if (frame > base) {
      popFrame(t);
      goto loop;
    } else {
      return 0;
    }
  } goto loop;

  case saload: {
    int32_t index = popInt(t);
    object array = popObject(t);

    if (LIKELY(array)) {
      if (LIKELY(index >= 0 and
                 static_cast<uintptr_t>(index) < shortArrayLength(t, array)))
      {
        pushInt(t, shortArrayBody(t, array, index));
      } else {
        object message = makeString(t, "%d not in [0,%d)", index,
                                    shortArrayLength(t, array));
        exception = makeArrayIndexOutOfBoundsException(t, message);
        goto throw_;
      }
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case sastore: {
    int16_t value = popInt(t);
    int32_t index = popInt(t);
    object array = popObject(t);

    if (LIKELY(array)) {
      if (LIKELY(index >= 0 and
                 static_cast<uintptr_t>(index) < shortArrayLength(t, array)))
      {
        shortArrayBody(t, array, index) = value;
      } else {
        object message = makeString(t, "%d not in [0,%d)", index,
                                    shortArrayLength(t, array));
        exception = makeArrayIndexOutOfBoundsException(t, message);
        goto throw_;
      }
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case sipush: {
    pushInt(t, static_cast<int16_t>(codeReadInt16(t, code, ip)));
  } goto loop;

  case swap: {
    uintptr_t tmp[2];
    memcpy(tmp                   , stack + ((sp - 1) * 2), BytesPerWord * 2);
    memcpy(stack + ((sp - 1) * 2), stack + ((sp - 2) * 2), BytesPerWord * 2);
    memcpy(stack + ((sp - 2) * 2), tmp                   , BytesPerWord * 2);
  } goto loop;

  case tableswitch: {
    int32_t base = ip - 1;

    ip += 3;
    ip -= (ip % 4);
    
    int32_t default_ = codeReadInt32(t, code, ip);
    int32_t bottom = codeReadInt32(t, code, ip);
    int32_t top = codeReadInt32(t, code, ip);
    
    int32_t key = popInt(t);
    
    if (key >= bottom and key <= top) {
      unsigned index = ip + ((key - bottom) * 4);
      ip = base + codeReadInt32(t, code, index);
    } else {
      ip = base + default_;      
    }
  } goto loop;

  case wide: goto wide;

  default: abort(t);
  }

 wide:
  switch (codeBody(t, code, ip++)) {
  case aload: {
    pushObject(t, localObject(t, codeReadInt16(t, code, ip)));
  } goto loop;

  case astore: {
    setLocalObject(t, codeReadInt16(t, code, ip), popObject(t));
  } goto loop;

  case iinc: {
    uint16_t index = codeReadInt16(t, code, ip);
    uint16_t count = codeReadInt16(t, code, ip);
    
    setLocalInt(t, index, localInt(t, index) + count);
  } goto loop;

  case iload: {
    pushInt(t, localInt(t, codeReadInt16(t, code, ip)));
  } goto loop;

  case istore: {
    setLocalInt(t, codeReadInt16(t, code, ip), popInt(t));
  } goto loop;

  case lload: {
    pushLong(t, localLong(t, codeReadInt16(t, code, ip)));
  } goto loop;

  case lstore: {
    setLocalLong(t, codeReadInt16(t, code, ip),  popLong(t));
  } goto loop;

  case ret: {
    ip = localInt(t, codeReadInt16(t, code, ip));
  } goto loop;

  default: abort(t);
  }

 invoke: {
    if (methodFlags(t, code) & ACC_NATIVE) {
      invokeNative(t, code);
      if (UNLIKELY(exception)) goto throw_;
    } else {
      checkStack(t, code);
      if (UNLIKELY(exception)) goto throw_;

      pushFrame(t, code);
    }
  } goto loop;

 throw_:
  if (DebugRun) {
    fprintf(stderr, "throw\n");
  }

  pokeInt(t, t->frame + FrameIpOffset, t->ip);
  for (; frame >= base; popFrame(t)) {
    ExceptionHandler* eh = findExceptionHandler(t, frame);
    if (eh) {
      sp = frame + FrameFootprint;
      ip = exceptionHandlerIp(eh);
      pushObject(t, exception);
      exception = 0;
      goto loop;
    }
  }

  return 0;
}

void
pushArguments(Thread* t, object this_, const char* spec, bool indirectObjects,
              va_list a)
{
  if (this_) {
    pushObject(t, this_);
  }

  const char* s = spec;
  ++ s; // skip '('
  while (*s and *s != ')') {
    switch (*s) {
    case 'L':
      while (*s and *s != ';') ++ s;
      ++ s;

      if (indirectObjects) {
        object* v = va_arg(a, object*);
        pushObject(t, v ? *v : 0);
      } else {
        pushObject(t, va_arg(a, object));
      }
      break;

    case '[':
      while (*s == '[') ++ s;
      switch (*s) {
      case 'L':
        while (*s and *s != ';') ++ s;
        ++ s;
        break;

      default:
        ++ s;
        break;
      }

      if (indirectObjects) {
        object* v = va_arg(a, object*);
        pushObject(t, v ? *v : 0);
      } else {
        pushObject(t, va_arg(a, object));
      }
      break;
      
    case 'J':
    case 'D':
      ++ s;
      pushLong(t, va_arg(a, uint64_t));
      break;

    case 'F': {
      ++ s;
      pushFloat(t, va_arg(a, double));
    } break;
          
    default:
      ++ s;
      pushInt(t, va_arg(a, uint32_t));
      break;
    }
  }
}

void
pushArguments(Thread* t, object this_, const char* spec, object a)
{
  if (this_) {
    pushObject(t, this_);
  }

  unsigned index = 0;
  const char* s = spec;
  ++ s; // skip '('
  while (*s and *s != ')') {
    switch (*s) {
    case 'L':
      while (*s and *s != ';') ++ s;
      ++ s;
      pushObject(t, objectArrayBody(t, a, index++));
      break;

    case '[':
      while (*s == '[') ++ s;
      switch (*s) {
      case 'L':
        while (*s and *s != ';') ++ s;
        ++ s;
        break;

      default:
        ++ s;
        break;
      }
      pushObject(t, objectArrayBody(t, a, index++));
      break;
      
    case 'J':
    case 'D':
      ++ s;
      pushLong(t, cast<int64_t>(objectArrayBody(t, a, index++), BytesPerWord));
      break;

    default:
      ++ s;
      pushInt(t, cast<int32_t>(objectArrayBody(t, a, index++), BytesPerWord));
      break;
    }
  }
}

inline unsigned
returnCode(Thread* t, object method)
{
  const char* s = reinterpret_cast<const char*>
    (&byteArrayBody(t, methodSpec(t, method), 0));
  while (*s and *s != ')') ++ s;
  return fieldCode(t, s[1]);
}

object
invoke(Thread* t, object method)
{
  PROTECT(t, method);

  object class_;
  PROTECT(t, class_);

  if (methodFlags(t, method) & ACC_STATIC) {
    class_ = methodClass(t, method);
  } else {
    unsigned parameterFootprint = methodParameterFootprint(t, method);
    class_ = objectClass(t, peekObject(t, t->sp - parameterFootprint));

    if (classFlags(t, methodClass(t, method)) & ACC_INTERFACE) {
      method = findInterfaceMethod(t, method, class_);
    } else {
      method = findMethod(t, method, class_);
    }
  }

  initClass(t, class_);

  object result = 0;

  if (methodFlags(t, method) & ACC_NATIVE) {
    unsigned returnCode = invokeNative(t, method);

    if (LIKELY(t->exception == 0)) {
      switch (returnCode) {
      case ByteField:
      case BooleanField:
      case CharField:
      case ShortField:
      case FloatField:
      case IntField:
        result = makeInt(t, popInt(t));
        break;

      case LongField:
      case DoubleField:
        result = makeLong(t, popLong(t));
        break;
        
      case ObjectField:
        result = popObject(t);
        break;

      case VoidField:
        result = 0;
        break;

      default:
        abort(t);
      };
    }
  } else {
    checkStack(t, method);
    if (LIKELY(t->exception == 0)) {
      pushFrame(t, method);
      result = interpret(t);
      if (LIKELY(t->exception == 0)) {
        popFrame(t);
      }
    }
  }

  if (UNLIKELY(t->exception)) {
    return 0;
  }

  switch (returnCode(t, method)) {
  case ByteField:
    return makeByte(t, static_cast<int8_t>(intValue(t, result)));

  case BooleanField:
    return makeBoolean(t, static_cast<uint8_t>(intValue(t, result)));

  case CharField:
    return makeChar(t, static_cast<uint16_t>(intValue(t, result)));

  case ShortField:
    return makeShort(t, static_cast<int16_t>(intValue(t, result)));

  case FloatField:
    return makeFloat(t, static_cast<uint32_t>(intValue(t, result)));

  case DoubleField:
    return makeDouble(t, static_cast<uint64_t>(longValue(t, result)));
        
  case ObjectField:
  case IntField:
  case LongField:
  case VoidField:
    return result;

  default:
    abort(t);
  }
}

class MyProcessor: public Processor {
 public:
  MyProcessor(System* s):
    s(s)
  { }

  virtual vm::Thread*
  makeThread(Machine* m, object javaThread, vm::Thread* parent)
  {
    return new (s->allocate(sizeof(Thread))) Thread(m, javaThread, parent);
  }

  virtual void*
  methodStub(vm::Thread*)
  {
    return 0;
  }

  virtual void*
  nativeInvoker(vm::Thread*)
  {
    return 0;
  }

  virtual unsigned
  parameterFootprint(vm::Thread*, const char* s, bool static_)
  {
    unsigned footprint = 0;
    ++ s; // skip '('
    while (*s and *s != ')') {
      switch (*s) {
      case 'L':
        while (*s and *s != ';') ++ s;
        ++ s;
        break;

      case '[':
        while (*s == '[') ++ s;
        switch (*s) {
        case 'L':
          while (*s and *s != ';') ++ s;
          ++ s;
          break;

        default:
          ++ s;
          break;
        }
        break;
      
      case 'J':
      case 'D':
        ++ s;
        ++ footprint;
        break;

      default:
        ++ s;
        break;
      }

      ++ footprint;
    }

    if (not static_) {
      ++ footprint;
    }
    return footprint;
  }

  virtual void
  initClass(vm::Thread* t, object c)
  {
    PROTECT(t, c);
    
    acquire(t, t->m->classLock);
    if (classVmFlags(t, c) & NeedInitFlag
        and (classVmFlags(t, c) & InitFlag) == 0)
    {
      classVmFlags(t, c) |= InitFlag;
      invoke(t, classInitializer(t, c), 0);
    } else {
      release(t, t->m->classLock);
    }
  }

  virtual void
  visitObjects(vm::Thread* vmt, Heap::Visitor* v)
  {
    Thread* t = static_cast<Thread*>(vmt);

    v->visit(&(t->code));

    for (unsigned i = 0; i < t->sp; ++i) {
      if (t->stack[i * 2] == ObjectTag) {
        v->visit(t->stack + (i * 2) + 1);
      }
    }
  }

  virtual uintptr_t
  frameStart(vm::Thread* vmt)
  {
    Thread* t = static_cast<Thread*>(vmt);

    if (t->frame >= 0) {
      pokeInt(t, t->frame + FrameIpOffset, t->ip);
    }
    return t->frame;
  }

  virtual uintptr_t
  frameNext(vm::Thread* vmt, uintptr_t frame)
  {
    Thread* t = static_cast<Thread*>(vmt);

    assert(t, static_cast<intptr_t>(frame) >= 0);
    return ::frameNext(t, frame);
  }

  virtual bool
  frameValid(vm::Thread*, uintptr_t frame)
  {
    return static_cast<intptr_t>(frame) >= 0;
  }

  virtual object
  frameMethod(vm::Thread* vmt, uintptr_t frame)
  {
    Thread* t = static_cast<Thread*>(vmt);

    assert(t, static_cast<intptr_t>(frame) >= 0);
    return ::frameMethod(t, frame);
  }

  virtual unsigned
  frameIp(vm::Thread* vmt, uintptr_t frame)
  {
    Thread* t = static_cast<Thread*>(vmt);

    assert(t, static_cast<intptr_t>(frame) >= 0);
    return ::frameIp(t, frame);
  }

  virtual int
  lineNumber(vm::Thread* vmt, object method, unsigned ip)
  {
    Thread* t = static_cast<Thread*>(vmt);

    if (methodFlags(t, method) & ACC_NATIVE) {
      return NativeLine;
    }

    object table = codeLineNumberTable(t, methodCode(t, method));
    if (table) {
      // todo: do a binary search:
      int last = UnknownLine;
      for (unsigned i = 0; i < lineNumberTableLength(t, table); ++i) {
        if (ip <= lineNumberIp(lineNumberTableBody(t, table, i))) {
          return last;
        } else {
          last = lineNumberLine(lineNumberTableBody(t, table, i));
        }
      }
      return last;
    } else {
      return UnknownLine;
    }
  }

  virtual object*
  makeLocalReference(vm::Thread* vmt, object o)
  {
    Thread* t = static_cast<Thread*>(vmt);

    return pushReference(t, o);
  }

  virtual void
  disposeLocalReference(vm::Thread*, object* r)
  {
    if (r) {
      *r = 0;
    }
  }

  virtual object
  invokeArray(vm::Thread* vmt, object method, object this_, object arguments)
  {
    Thread* t = static_cast<Thread*>(vmt);

    assert(t, t->state == Thread::ActiveState
           or t->state == Thread::ExclusiveState);

    assert(t, ((methodFlags(t, method) & ACC_STATIC) == 0) xor (this_ == 0));

    if (UNLIKELY(t->sp + methodParameterFootprint(t, method) + 1
                 > Thread::StackSizeInWords / 2))
    {
      t->exception = makeStackOverflowError(t);
      return 0;
    }

    const char* spec = reinterpret_cast<char*>
      (&byteArrayBody(t, methodSpec(t, method), 0));
    pushArguments(t, this_, spec, arguments);

    return ::invoke(t, method);
  }

  virtual object
  invokeList(vm::Thread* vmt, object method, object this_,
             bool indirectObjects, va_list arguments)
  {
    Thread* t = static_cast<Thread*>(vmt);

    assert(t, t->state == Thread::ActiveState
           or t->state == Thread::ExclusiveState);

    assert(t, ((methodFlags(t, method) & ACC_STATIC) == 0) xor (this_ == 0));

    if (UNLIKELY(t->sp + methodParameterFootprint(t, method) + 1
                 > Thread::StackSizeInWords / 2))
    {
      t->exception = makeStackOverflowError(t);
      return 0;
    }

    const char* spec = reinterpret_cast<char*>
      (&byteArrayBody(t, methodSpec(t, method), 0));
    pushArguments(t, this_, spec, indirectObjects, arguments);

    return ::invoke(t, method);
  }

  virtual object
  invokeList(vm::Thread* vmt, const char* className, const char* methodName,
             const char* methodSpec, object this_, va_list arguments)
  {
    Thread* t = static_cast<Thread*>(vmt);

    assert(t, t->state == Thread::ActiveState
           or t->state == Thread::ExclusiveState);

    if (UNLIKELY(t->sp + parameterFootprint(vmt, methodSpec, false)
                 > Thread::StackSizeInWords / 2))
    {
      t->exception = makeStackOverflowError(t);
      return 0;
    }

    pushArguments(t, this_, methodSpec, false, arguments);

    object method = resolveMethod(t, className, methodName, methodSpec);
    if (LIKELY(t->exception == 0)) {
      assert(t, ((methodFlags(t, method) & ACC_STATIC) == 0) xor (this_ == 0));

      return ::invoke(t, method);
    } else {
      return 0;
    }
  }

  virtual void dispose() {
    s->free(this);
  }
  
  System* s;
};

} // namespace

namespace vm {

Processor*
makeProcessor(System* system)
{
  return new (system->allocate(sizeof(MyProcessor))) MyProcessor(system);
}

} // namespace vm
