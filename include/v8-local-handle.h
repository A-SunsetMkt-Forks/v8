// Copyright 2021 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef INCLUDE_V8_LOCAL_HANDLE_H_
#define INCLUDE_V8_LOCAL_HANDLE_H_

#include <stddef.h>

#include <type_traits>
#include <vector>

#include "v8-handle-base.h"  // NOLINT(build/include_directory)
#include "v8-internal.h"     // NOLINT(build/include_directory)

namespace v8 {

template <class T>
class LocalBase;
template <class T>
class Local;
template <class T>
class LocalVector;
template <class F>
class MaybeLocal;

template <class T>
class Eternal;
template <class T>
class Global;

template <class T>
class NonCopyablePersistentTraits;
template <class T>
class PersistentBase;
template <class T, class M = NonCopyablePersistentTraits<T>>
class Persistent;

class TracedReferenceBase;
template <class T>
class BasicTracedReference;
template <class F>
class TracedReference;

class ArrayBuffer;
class Boolean;
class Context;
class EscapableHandleScope;
template <class F>
class FunctionCallbackInfo;
class Isolate;
class Object;
template <class F1, class F2, class F3>
class PersistentValueMapBase;
class Primitive;
class Private;
template <class F>
class PropertyCallbackInfo;
template <class F>
class ReturnValue;
class String;
template <class F>
class Traced;
class TypecheckWitness;
class Utils;
class Uint32;
class Value;

namespace debug {
class ConsoleCallArguments;
}

namespace internal {
template <typename T>
class CustomArguments;
template <typename T>
class LocalUnchecked;
class SamplingHeapProfiler;
}  // namespace internal

namespace api_internal {
// Called when ToLocalChecked is called on an empty Local.
V8_EXPORT void ToLocalEmpty();

#ifdef V8_ENABLE_CHECKS
template <typename T, typename V = Value>
void TypeCheckLocal(V* value) {
  // If `T` does not provide a `Cast` method we cannot check anything.
  if constexpr (requires { T::Cast(value); }) {
    // TODO(419454582): Remove all these exceptions.
    if (std::is_same_v<Array, T> && value->IsArgumentsObject()) return;
    if (std::is_same_v<ArrayBuffer, T> && value->IsSharedArrayBuffer()) return;
    if (std::is_same_v<Object, T> && value->IsNull()) return;
    if (std::is_same_v<Object, T> && value->IsString()) return;
    if (std::is_same_v<Object, T> && value->IsUndefined()) return;
    if (std::is_same_v<Uint32, T> && value->IsInt32()) return;
    if (std::is_same_v<Object, T> && value->IsNumber()) return;
    // Execute the actual check (part of the cast).
    T::Cast(value);
  }
}
#endif
}  // namespace api_internal

/**
 * A stack-allocated class that governs a number of local handles.
 * After a handle scope has been created, all local handles will be
 * allocated within that handle scope until either the handle scope is
 * deleted or another handle scope is created.  If there is already a
 * handle scope and a new one is created, all allocations will take
 * place in the new handle scope until it is deleted.  After that,
 * new handles will again be allocated in the original handle scope.
 *
 * After the handle scope of a local handle has been deleted the
 * garbage collector will no longer track the object stored in the
 * handle and may deallocate it.  The behavior of accessing a handle
 * for which the handle scope has been deleted is undefined.
 */
class V8_EXPORT V8_NODISCARD HandleScope {
 public:
  V8_INLINE explicit HandleScope(Isolate* isolate);

  V8_INLINE ~HandleScope();

  /**
   * Counts the number of allocated handles.
   */
  static int NumberOfHandles(Isolate* isolate);

  V8_INLINE Isolate* GetIsolate() const { return isolate_; }

  HandleScope(const HandleScope&) = delete;
  void operator=(const HandleScope&) = delete;

  static internal::Address* CreateHandleForCurrentIsolate(
      internal::Address value);

 protected:
  V8_INLINE HandleScope() = default;

  V8_INLINE void Initialize(Isolate* isolate);

  V8_INLINE static internal::Address* CreateHandle(Isolate* i_isolate,
                                                   internal::Address value);

 private:
  // Extend the HandleScope making room for more handles.  Not inlined.
  static internal::Address* Extend(Isolate* isolate);
  // Delete any extensions in HandleScope destructor.  Not called unless there
  // are extensions.  Not inlined.
  void DeleteExtensions(Isolate* isolate);

#ifdef V8_ENABLE_CHECKS
  // Non-inlined asserts on HandleScope constructor.
  void DoInitializeAsserts(Isolate* isolate);
  // Non-inlined assert for HandleScope destructor.
  void AssertScopeLevelsMatch();
  // Non-inlined asserts for HandleScope destructor.  Also zaps the slots
  // if this is enabled.
  void DoCloseScopeAsserts(int before, internal::Address* limit,
                           internal::HandleScopeData* current);
#endif

  // Declaring operator new and delete as deleted is not spec compliant.
  // Therefore declare them private instead to disable dynamic alloc
  void* operator new(size_t size);
  void* operator new[](size_t size);
  void operator delete(void*, size_t);
  void operator delete[](void*, size_t);

  Isolate* isolate_;
  internal::Address* prev_next_;
  internal::Address* prev_limit_;
#ifdef V8_ENABLE_CHECKS
  int scope_level_ = 0;
#endif

  // LocalBase<T>::New uses CreateHandle with an Isolate* parameter.
  template <typename T>
  friend class LocalBase;

  // Object::GetInternalField and Context::GetEmbedderData use CreateHandle with
  // a HeapObject in their shortcuts.
  friend class Object;
  friend class Context;
};

HandleScope::HandleScope(Isolate* v8_isolate) { Initialize(v8_isolate); }

void HandleScope::Initialize(Isolate* v8_isolate) {
  using I = internal::Internals;
  internal::HandleScopeData* current = I::GetHandleScopeData(v8_isolate);
  isolate_ = v8_isolate;
  prev_next_ = current->next;
  prev_limit_ = current->limit;
  current->level++;
#ifdef V8_ENABLE_CHECKS
  DoInitializeAsserts(v8_isolate);
  scope_level_ = current->level;
#endif
}

HandleScope::~HandleScope() {
  if (V8_UNLIKELY(isolate_ == nullptr)) return;
#ifdef V8_ENABLE_CHECKS
  AssertScopeLevelsMatch();
  int handle_count_before = NumberOfHandles(isolate_);
#endif

  using I = internal::Internals;
  internal::HandleScopeData* current = I::GetHandleScopeData(isolate_);
  std::swap(current->next, prev_next_);
  current->level--;
  internal::Address* limit = prev_next_;
  if (V8_UNLIKELY(current->limit != prev_limit_)) {
    current->limit = prev_limit_;
    limit = prev_limit_;
    DeleteExtensions(isolate_);
  }
#ifdef V8_ENABLE_CHECKS
  DoCloseScopeAsserts(handle_count_before, limit, current);
#else
  (void)limit;  // Avoid unused variable warning.
#endif
}

internal::Address* HandleScope::CreateHandle(Isolate* v8_isolate,
                                             internal::Address value) {
  using I = internal::Internals;
  internal::HandleScopeData* data = I::GetHandleScopeData(v8_isolate);
  internal::Address* result = data->next;
  if (V8_UNLIKELY(result == data->limit)) {
    result = Extend(v8_isolate);
  }
  // Update the current next field, set the value in the created handle,
  // and return the result.
  data->next = reinterpret_cast<internal::Address*>(
      reinterpret_cast<internal::Address>(result) + sizeof(internal::Address));
  *result = value;
  return result;
}

/**
 * A base class for local handles.
 * Its implementation depends on whether direct handle support is enabled.
 * When it is, a local handle contains a direct pointer to the referenced
 * object, otherwise it contains an indirect pointer.
 */
#ifdef V8_ENABLE_DIRECT_HANDLE

template <typename T>
class LocalBase : public api_internal::DirectHandleBase {
 protected:
  template <class F>
  friend class Local;

  V8_INLINE LocalBase() = default;

  V8_INLINE explicit LocalBase(internal::Address ptr) : DirectHandleBase(ptr) {
#ifdef V8_ENABLE_CHECKS
    if (!IsEmpty()) api_internal::TypeCheckLocal<T>(value<Value>());
#endif
  }

  template <typename S>
  V8_INLINE LocalBase(const LocalBase<S>& other) : DirectHandleBase(other) {}

  V8_INLINE static LocalBase<T> New(Isolate* isolate, internal::Address value) {
    return LocalBase<T>(value);
  }

  V8_INLINE static LocalBase<T> New(Isolate* isolate, T* that) {
    return LocalBase<T>::New(isolate,
                             internal::ValueHelper::ValueAsAddress(that));
  }

  V8_INLINE static LocalBase<T> FromSlot(internal::Address* slot) {
    if (slot == nullptr) return LocalBase<T>();
    return LocalBase<T>(*slot);
  }

  V8_INLINE static LocalBase<T> FromRepr(
      internal::ValueHelper::InternalRepresentationType repr) {
    return LocalBase<T>(repr);
  }
};

#else  // !V8_ENABLE_DIRECT_HANDLE

template <typename T>
class LocalBase : public api_internal::IndirectHandleBase {
 protected:
  template <class F>
  friend class Local;

  V8_INLINE LocalBase() = default;

  V8_INLINE explicit LocalBase(internal::Address* location)
      : IndirectHandleBase(location) {
#ifdef V8_ENABLE_CHECKS
    if (!IsEmpty()) api_internal::TypeCheckLocal<T>(value<Value>());
#endif
  }

  template <typename S>
  V8_INLINE LocalBase(const LocalBase<S>& other) : IndirectHandleBase(other) {}

  V8_INLINE static LocalBase<T> New(Isolate* isolate, internal::Address value) {
    return LocalBase(HandleScope::CreateHandle(isolate, value));
  }

  V8_INLINE static LocalBase<T> New(Isolate* isolate, T* that) {
    if (internal::ValueHelper::IsEmpty(that)) return LocalBase<T>();
    return LocalBase<T>::New(isolate,
                             internal::ValueHelper::ValueAsAddress(that));
  }

  V8_INLINE static LocalBase<T> FromSlot(internal::Address* slot) {
    return LocalBase<T>(slot);
  }

  V8_INLINE static LocalBase<T> FromRepr(
      internal::ValueHelper::InternalRepresentationType repr) {
    return LocalBase<T>(repr);
  }
};

#endif  // V8_ENABLE_DIRECT_HANDLE

/**
 * An object reference managed by the v8 garbage collector.
 *
 * All objects returned from v8 have to be tracked by the garbage collector so
 * that it knows that the objects are still alive.  Also, because the garbage
 * collector may move objects, it is unsafe to point directly to an object.
 * Instead, all objects are stored in handles which are known by the garbage
 * collector and updated whenever an object moves.  Handles should always be
 * passed by value (except in cases like out-parameters) and they should never
 * be allocated on the heap.
 *
 * There are two types of handles: local and persistent handles.
 *
 * Local handles are light-weight and transient and typically used in local
 * operations.  They are managed by HandleScopes. That means that a HandleScope
 * must exist on the stack when they are created and that they are only valid
 * inside of the HandleScope active during their creation. For passing a local
 * handle to an outer HandleScope, an EscapableHandleScope and its Escape()
 * method must be used.
 *
 * Persistent handles can be used when storing objects across several
 * independent operations and have to be explicitly deallocated when they're no
 * longer used.
 *
 * It is safe to extract the object stored in the handle by dereferencing the
 * handle (for instance, to extract the Object* from a Local<Object>); the value
 * will still be governed by a handle behind the scenes and the same rules apply
 * to these values as to their handles.
 */
template <class T>
class V8_TRIVIAL_ABI Local : public LocalBase<T>,
#ifdef V8_ENABLE_LOCAL_OFF_STACK_CHECK
                             public api_internal::StackAllocated<true>
#else
                             public api_internal::StackAllocated<false>
#endif
{
 public:
  /**
   * Default constructor: Returns an empty handle.
   */
  V8_INLINE Local() = default;

  /**
   * Constructor for handling automatic up casting.
   * Ex. Local<Object> can be passed when Local<Value> is expected but not
   * the other way round.
   */
  template <class S>
    requires std::is_base_of_v<T, S>
  V8_INLINE Local(Local<S> that) : LocalBase<T>(that) {}

  V8_INLINE T* operator->() const { return this->template value<T>(); }

  V8_INLINE T* operator*() const { return this->operator->(); }

  /**
   * Checks whether two handles are equal or different.
   * They are equal iff they are both empty or they are both non-empty and the
   * objects to which they refer are physically equal.
   *
   * If both handles refer to JS objects, this is the same as strict
   * non-equality. For primitives, such as numbers or strings, a `true` return
   * value does not indicate that the values aren't equal in the JavaScript
   * sense. Use `Value::StrictEquals()` to check primitives for equality.
   */

  template <class S>
  V8_INLINE bool operator==(const Local<S>& that) const {
    return internal::HandleHelper::EqualHandles(*this, that);
  }

  template <class S>
  V8_INLINE bool operator==(const PersistentBase<S>& that) const {
    return internal::HandleHelper::EqualHandles(*this, that);
  }

  template <class S>
  V8_INLINE bool operator!=(const Local<S>& that) const {
    return !operator==(that);
  }

  template <class S>
  V8_INLINE bool operator!=(const Persistent<S>& that) const {
    return !operator==(that);
  }

  /**
   * Cast a handle to a subclass, e.g. Local<Value> to Local<Object>.
   * This is only valid if the handle actually refers to a value of the
   * target type or if the handle is empty.
   */
  template <class S>
  V8_INLINE static Local<T> Cast(Local<S> that) {
#ifdef V8_ENABLE_CHECKS
    // If we're going to perform the type check then we have to check
    // that the handle isn't empty before doing the checked cast.
    if (that.IsEmpty()) return Local<T>();
    T::Cast(that.template value<S>());
#endif
    return Local<T>(LocalBase<T>(that));
  }

  /**
   * Calling this is equivalent to Local<S>::Cast().
   * In particular, this is only valid if the handle actually refers to a value
   * of the target type or if the handle is empty.
   */
  template <class S>
  V8_INLINE Local<S> As() const {
    return Local<S>::Cast(*this);
  }

  /**
   * Create a local handle for the content of another handle.
   * The referee is kept alive by the local handle even when
   * the original handle is destroyed/disposed.
   */
  V8_INLINE static Local<T> New(Isolate* isolate, Local<T> that) {
    return New(isolate, that.template value<T, true>());
  }

  V8_INLINE static Local<T> New(Isolate* isolate,
                                const PersistentBase<T>& that) {
    return New(isolate, that.template value<T, true>());
  }

  V8_INLINE static Local<T> New(Isolate* isolate,
                                const BasicTracedReference<T>& that) {
    return New(isolate, that.template value<T, true>());
  }

 private:
  friend class TracedReferenceBase;
  friend class Utils;
  template <class F>
  friend class Eternal;
  template <class F>
  friend class Global;
  template <class F>
  friend class Local;
  template <class F>
  friend class MaybeLocal;
  template <class F, class M>
  friend class Persistent;
  template <class F>
  friend class FunctionCallbackInfo;
  template <class F>
  friend class PropertyCallbackInfo;
  friend class String;
  friend class Object;
  friend class Context;
  friend class Isolate;
  friend class Private;
  template <class F>
  friend class internal::CustomArguments;
  friend Local<Primitive> Undefined(Isolate* isolate);
  friend Local<Primitive> Null(Isolate* isolate);
  friend Local<Boolean> True(Isolate* isolate);
  friend Local<Boolean> False(Isolate* isolate);
  friend class HandleScope;
  friend class EscapableHandleScope;
  friend class InternalEscapableScope;
  template <class F1, class F2, class F3>
  friend class PersistentValueMapBase;
  template <class F>
  friend class ReturnValue;
  template <class F>
  friend class Traced;
  friend class internal::SamplingHeapProfiler;
  friend class internal::HandleHelper;
  friend class debug::ConsoleCallArguments;
  friend class internal::LocalUnchecked<T>;

  explicit Local(no_checking_tag do_not_check)
      : LocalBase<T>(), StackAllocated(do_not_check) {}
  explicit Local(const Local<T>& other, no_checking_tag do_not_check)
      : LocalBase<T>(other), StackAllocated(do_not_check) {}

  V8_INLINE explicit Local(const LocalBase<T>& other) : LocalBase<T>(other) {}

  V8_INLINE static Local<T> FromRepr(
      internal::ValueHelper::InternalRepresentationType repr) {
    return Local<T>(LocalBase<T>::FromRepr(repr));
  }

  V8_INLINE static Local<T> FromSlot(internal::Address* slot) {
    return Local<T>(LocalBase<T>::FromSlot(slot));
  }

#ifdef V8_ENABLE_DIRECT_HANDLE
  friend class TypecheckWitness;

  V8_INLINE static Local<T> FromAddress(internal::Address ptr) {
    return Local<T>(LocalBase<T>(ptr));
  }
#endif  // V8_ENABLE_DIRECT_HANDLE

  V8_INLINE static Local<T> New(Isolate* isolate, internal::Address value) {
    return Local<T>(LocalBase<T>::New(isolate, value));
  }

  V8_INLINE static Local<T> New(Isolate* isolate, T* that) {
    return Local<T>(LocalBase<T>::New(isolate, that));
  }

  // Unsafe cast, should be avoided.
  template <class S>
  V8_INLINE Local<S> UnsafeAs() const {
    return Local<S>(LocalBase<S>(*this));
  }
};

namespace internal {
// A local variant that is suitable for off-stack allocation.
// Used internally by LocalVector<T>. Not to be used directly!
template <typename T>
class V8_TRIVIAL_ABI LocalUnchecked : public Local<T> {
 public:
  LocalUnchecked() : Local<T>(Local<T>::do_not_check) {}

#if defined(V8_ENABLE_LOCAL_OFF_STACK_CHECK) && V8_HAS_ATTRIBUTE_TRIVIAL_ABI
  // In this case, the check is also enforced in the copy constructor and we
  // need to suppress it.
  LocalUnchecked(
      const LocalUnchecked& other) noexcept  // NOLINT(runtime/explicit)
      : Local<T>(other, Local<T>::do_not_check) {}
  LocalUnchecked& operator=(const LocalUnchecked&) noexcept = default;
#endif

  // Implicit conversion from Local.
  LocalUnchecked(const Local<T>& other) noexcept  // NOLINT(runtime/explicit)
      : Local<T>(other, Local<T>::do_not_check) {}
};

#ifdef V8_ENABLE_DIRECT_HANDLE
// Off-stack allocated direct locals must be registered as strong roots.
// For off-stack indirect locals, this is not necessary.

template <typename T>
class StrongRootAllocator<LocalUnchecked<T>> : public StrongRootAllocatorBase {
 public:
  using value_type = LocalUnchecked<T>;
  static_assert(std::is_standard_layout_v<value_type>);
  static_assert(sizeof(value_type) == sizeof(Address));

  template <typename HeapOrIsolateT>
  explicit StrongRootAllocator(HeapOrIsolateT* heap_or_isolate)
      : StrongRootAllocatorBase(heap_or_isolate) {}
  template <typename U>
  StrongRootAllocator(const StrongRootAllocator<U>& other) noexcept
      : StrongRootAllocatorBase(other) {}

  value_type* allocate(size_t n) {
    return reinterpret_cast<value_type*>(allocate_impl(n));
  }
  void deallocate(value_type* p, size_t n) noexcept {
    return deallocate_impl(reinterpret_cast<Address*>(p), n);
  }
};
#endif  // V8_ENABLE_DIRECT_HANDLE
}  // namespace internal

template <typename T>
class LocalVector {
 private:
  using element_type = internal::LocalUnchecked<T>;

#ifdef V8_ENABLE_DIRECT_HANDLE
  using allocator_type = internal::StrongRootAllocator<element_type>;

  static allocator_type make_allocator(Isolate* isolate) noexcept {
    return allocator_type(isolate);
  }
#else
  using allocator_type = std::allocator<element_type>;

  static allocator_type make_allocator(Isolate* isolate) noexcept {
    return allocator_type();
  }
#endif  // V8_ENABLE_DIRECT_HANDLE

  using vector_type = std::vector<element_type, allocator_type>;

 public:
  using value_type = Local<T>;
  using reference = value_type&;
  using const_reference = const value_type&;
  using size_type = size_t;
  using difference_type = ptrdiff_t;
  using iterator =
      internal::WrappedIterator<typename vector_type::iterator, Local<T>>;
  using const_iterator =
      internal::WrappedIterator<typename vector_type::const_iterator,
                                const Local<T>>;

  explicit LocalVector(Isolate* isolate) : backing_(make_allocator(isolate)) {}
  LocalVector(Isolate* isolate, size_t n)
      : backing_(n, make_allocator(isolate)) {}
  explicit LocalVector(Isolate* isolate, std::initializer_list<Local<T>> init)
      : backing_(make_allocator(isolate)) {
    if (init.size() == 0) return;
    backing_.reserve(init.size());
    backing_.insert(backing_.end(), init.begin(), init.end());
  }

  iterator begin() noexcept { return iterator(backing_.begin()); }
  const_iterator begin() const noexcept {
    return const_iterator(backing_.begin());
  }
  iterator end() noexcept { return iterator(backing_.end()); }
  const_iterator end() const noexcept { return const_iterator(backing_.end()); }

  size_t size() const noexcept { return backing_.size(); }
  bool empty() const noexcept { return backing_.empty(); }
  void reserve(size_t n) { backing_.reserve(n); }
  void shrink_to_fit() { backing_.shrink_to_fit(); }

  Local<T>& operator[](size_t n) { return backing_[n]; }
  const Local<T>& operator[](size_t n) const { return backing_[n]; }

  Local<T>& at(size_t n) { return backing_.at(n); }
  const Local<T>& at(size_t n) const { return backing_.at(n); }

  Local<T>& front() { return backing_.front(); }
  const Local<T>& front() const { return backing_.front(); }
  Local<T>& back() { return backing_.back(); }
  const Local<T>& back() const { return backing_.back(); }

  Local<T>* data() noexcept { return backing_.data(); }
  const Local<T>* data() const noexcept { return backing_.data(); }

  iterator insert(const_iterator pos, const Local<T>& value) {
    return iterator(backing_.insert(pos.base(), value));
  }

  template <typename InputIt>
  iterator insert(const_iterator pos, InputIt first, InputIt last) {
    return iterator(backing_.insert(pos.base(), first, last));
  }

  iterator insert(const_iterator pos, std::initializer_list<Local<T>> init) {
    return iterator(backing_.insert(pos.base(), init.begin(), init.end()));
  }

  LocalVector<T>& operator=(std::initializer_list<Local<T>> init) {
    backing_.clear();
    backing_.reserve(init.size());
    backing_.insert(backing_.end(), init.begin(), init.end());
    return *this;
  }

  void push_back(const Local<T>& x) { backing_.push_back(x); }
  void pop_back() { backing_.pop_back(); }

  template <typename... Args>
  void emplace_back(Args&&... args) {
    backing_.push_back(value_type{std::forward<Args>(args)...});
  }

  void clear() noexcept { backing_.clear(); }
  void resize(size_t n) { backing_.resize(n); }
  void swap(LocalVector<T>& other) { backing_.swap(other.backing_); }

  friend bool operator==(const LocalVector<T>& x, const LocalVector<T>& y) {
    return x.backing_ == y.backing_;
  }
  friend bool operator!=(const LocalVector<T>& x, const LocalVector<T>& y) {
    return x.backing_ != y.backing_;
  }
  friend bool operator<(const LocalVector<T>& x, const LocalVector<T>& y) {
    return x.backing_ < y.backing_;
  }
  friend bool operator>(const LocalVector<T>& x, const LocalVector<T>& y) {
    return x.backing_ > y.backing_;
  }
  friend bool operator<=(const LocalVector<T>& x, const LocalVector<T>& y) {
    return x.backing_ <= y.backing_;
  }
  friend bool operator>=(const LocalVector<T>& x, const LocalVector<T>& y) {
    return x.backing_ >= y.backing_;
  }

 private:
  vector_type backing_;
};

#if !defined(V8_IMMINENT_DEPRECATION_WARNINGS)
// Handle is an alias for Local for historical reasons.
template <class T>
using Handle = Local<T>;
#endif

/**
 * A MaybeLocal<> is a wrapper around Local<> that enforces a check whether
 * the Local<> is empty before it can be used.
 *
 * If an API method returns a MaybeLocal<>, the API method can potentially fail
 * either because an exception is thrown, or because an exception is pending,
 * e.g. because a previous API call threw an exception that hasn't been caught
 * yet, or because a TerminateExecution exception was thrown. In that case, an
 * empty MaybeLocal is returned.
 */
template <class T>
class MaybeLocal {
 public:
  /**
   * Default constructor: Returns an empty handle.
   */
  V8_INLINE MaybeLocal() = default;
  /**
   * Implicitly construct MaybeLocal from Local.
   */
  template <class S>
    requires std::is_base_of_v<T, S>
  V8_INLINE MaybeLocal(Local<S> that) : local_(that) {}
  /**
   * Implicitly up-cast MaybeLocal<S> to MaybeLocal<T> if T is a base of S.
   */
  template <class S>
    requires std::is_base_of_v<T, S>
  V8_INLINE MaybeLocal(MaybeLocal<S> that) : local_(that.local_) {}

  V8_INLINE bool IsEmpty() const { return local_.IsEmpty(); }

  /**
   * Converts this MaybeLocal<> to a Local<>. If this MaybeLocal<> is empty,
   * |false| is returned and |out| is assigned with nullptr.
   */
  template <class S>
  V8_WARN_UNUSED_RESULT V8_INLINE bool ToLocal(Local<S>* out) const {
    *out = local_;
    return !IsEmpty();
  }

  /**
   * Converts this MaybeLocal<> to a Local<>. If this MaybeLocal<> is empty,
   * V8 will crash the process.
   */
  V8_INLINE Local<T> ToLocalChecked() {
    if (V8_UNLIKELY(IsEmpty())) api_internal::ToLocalEmpty();
    return local_;
  }

  /**
   * Converts this MaybeLocal<> to a Local<>, using a default value if this
   * MaybeLocal<> is empty.
   */
  template <class S>
  V8_INLINE Local<S> FromMaybe(Local<S> default_value) const {
    return IsEmpty() ? default_value : Local<S>(local_);
  }

  /**
   * Cast a handle to a subclass, e.g. MaybeLocal<Value> to MaybeLocal<Object>.
   * This is only valid if the handle actually refers to a value of the target
   * type or if the handle is empty.
   */
  template <class S>
  V8_INLINE static MaybeLocal<T> Cast(MaybeLocal<S> that) {
    return MaybeLocal<T>{Local<T>::Cast(that.local_)};
  }

  /**
   * Calling this is equivalent to MaybeLocal<S>::Cast().
   * In particular, this is only valid if the handle actually refers to a value
   * of the target type or if the handle is empty.
   */
  template <class S>
  V8_INLINE MaybeLocal<S> As() const {
    return MaybeLocal<S>::Cast(*this);
  }

 private:
  Local<T> local_;

  template <typename S>
  friend class MaybeLocal;
};

/**
 * A HandleScope which first allocates a handle in the current scope
 * which will be later filled with the escape value.
 */
class V8_EXPORT V8_NODISCARD EscapableHandleScopeBase : public HandleScope {
 public:
  explicit EscapableHandleScopeBase(Isolate* isolate);
  V8_INLINE ~EscapableHandleScopeBase() = default;

  EscapableHandleScopeBase(const EscapableHandleScopeBase&) = delete;
  void operator=(const EscapableHandleScopeBase&) = delete;
  void* operator new(size_t size) = delete;
  void* operator new[](size_t size) = delete;
  void operator delete(void*, size_t) = delete;
  void operator delete[](void*, size_t) = delete;

 protected:
  /**
   * Pushes the value into the previous scope and returns a handle to it.
   * Cannot be called twice.
   */
  internal::Address* EscapeSlot(internal::Address* escape_value);

 private:
  internal::Address* escape_slot_;
};

class V8_EXPORT V8_NODISCARD EscapableHandleScope
    : public EscapableHandleScopeBase {
 public:
  explicit EscapableHandleScope(Isolate* isolate)
      : EscapableHandleScopeBase(isolate) {}
  V8_INLINE ~EscapableHandleScope() = default;
  template <class T>
  V8_INLINE Local<T> Escape(Local<T> value) {
#ifdef V8_ENABLE_DIRECT_HANDLE
    return value;
#else
    if (value.IsEmpty()) return value;
    return Local<T>::FromSlot(EscapeSlot(value.slot()));
#endif
  }

  template <class T>
  V8_INLINE MaybeLocal<T> EscapeMaybe(MaybeLocal<T> value) {
    return Escape(value.FromMaybe(Local<T>()));
  }
};

/**
 * A SealHandleScope acts like a handle scope in which no handle allocations
 * are allowed. It can be useful for debugging handle leaks.
 * Handles can be allocated within inner normal HandleScopes.
 */
class V8_EXPORT V8_NODISCARD SealHandleScope {
 public:
  explicit SealHandleScope(Isolate* isolate);
  ~SealHandleScope();

  SealHandleScope(const SealHandleScope&) = delete;
  void operator=(const SealHandleScope&) = delete;
  void* operator new(size_t size) = delete;
  void* operator new[](size_t size) = delete;
  void operator delete(void*, size_t) = delete;
  void operator delete[](void*, size_t) = delete;

 private:
  internal::Isolate* const i_isolate_;
  internal::Address* prev_limit_;
  int prev_sealed_level_;
};

}  // namespace v8

#endif  // INCLUDE_V8_LOCAL_HANDLE_H_
