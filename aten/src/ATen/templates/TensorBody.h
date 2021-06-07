#pragma once

#include <c10/core/Device.h>
#include <c10/core/Layout.h>
#include <c10/core/MemoryFormat.h>
#include <c10/core/QScheme.h>
#include <c10/core/Stream.h>
#include <c10/core/Scalar.h>
#include <c10/core/ScalarType.h>
#include <c10/core/ScalarTypeToTypeMeta.h>
#include <c10/core/Storage.h>
#include <ATen/core/TensorAccessor.h>
#include <c10/core/TensorImpl.h>
#include <c10/core/UndefinedTensorImpl.h>
#include <c10/core/WrapDimMinimal.h>
#include <c10/util/Exception.h>
#include <c10/util/Deprecated.h>
#include <c10/util/MaybeOwned.h>
#include <c10/util/Optional.h>
#include <c10/util/intrusive_ptr.h>
#include <ATen/core/DeprecatedTypePropertiesRegistry.h>
#include <ATen/core/DeprecatedTypeProperties.h>
#include <ATen/core/NamedTensor.h>
#include <ATen/core/QuantizerBase.h>
#include <torch/csrc/WindowsTorchApiMacro.h>

namespace caffe2 {
class Tensor;
}
namespace c10{
struct TensorOptions;
template<class T> class List;
}
namespace at {
struct Generator;
struct Type;
class DeprecatedTypeProperties;
class Tensor;
} // namespace at
namespace at {
namespace indexing {
struct TensorIndex;
} // namespace indexing
} // namespace at

namespace torch { namespace autograd {

struct Node;

}} // namespace torch::autograd

namespace at {

class Tensor;
using TensorList = ArrayRef<Tensor>;

using Stream = c10::Stream;

namespace impl {
inline bool variable_excluded_from_dispatch() {
#ifdef C10_MOBILE
  // Please read the comment in `VariableFallbackKernel.cpp` about the background of this change.
  return true;
#else
  TORCH_INTERNAL_ASSERT_DEBUG_ONLY(!c10::impl::LocalDispatchKeySetWrapper().excluded().has(DispatchKey::Autograd));
  return c10::impl::LocalDispatchKeySetWrapper().excluded().isSupersetOf(c10::autograd_dispatch_keyset);
#endif
}
}

// Tensor is a "generic" object holding a pointer to the underlying TensorImpl object, which
// has an embedded reference count. In this way, Tensor is similar to boost::intrusive_ptr.
//
// For example:
//
// void func(Tensor a) {
//   Tensor b = a;
//   ...
// }
//
// In this example, when we say Tensor b = a, we are creating a new object that points to the
// same underlying TensorImpl, and bumps its reference count. When b goes out of scope, the
// destructor decrements the reference count by calling release() on the TensorImpl it points to.
// The existing constructors, operator overloads, etc. take care to implement the correct semantics.
//
// Note that Tensor can also be NULL, i.e. it is not associated with any underlying TensorImpl, and
// special care must be taken to handle this.
class TORCH_API Tensor {
 private:
  struct unsafe_borrow_t { explicit unsafe_borrow_t() = default; };

  // Create a Tensor with a +0 reference count. Special care must be
  // taken to avoid decrementing this reference count at destruction
  // time. Intended to support MaybeOwnedTraits<Tensor>.
  explicit Tensor(unsafe_borrow_t, const Tensor& rhs)
      : impl_(c10::intrusive_ptr<at::TensorImpl, UndefinedTensorImpl>::reclaim(rhs.impl_.get())) {}
  friend MaybeOwnedTraits<Tensor>;

 public:
  Tensor(){};
  // This constructor should not be used by end users and is an implementation
  // detail invoked by autogenerated code.
  explicit Tensor(
      c10::intrusive_ptr<TensorImpl, UndefinedTensorImpl> tensor_impl)
      : impl_(std::move(tensor_impl)) {
    if (impl_.get() == nullptr) {
      throw std::runtime_error("TensorImpl with nullptr is not supported");
    }
  }
  Tensor(const Tensor&) = default;
  Tensor(Tensor&&) = default;


 public:
  // Creates a new wrapper from TensorImpl. Intentionally a free method because
  // it should be used with care. Checks necessary invariants
  static Tensor wrap_tensor_impl(
      c10::intrusive_ptr<TensorImpl, UndefinedTensorImpl> tensor_impl) {
    Tensor r(std::move(tensor_impl));
    r.enforce_invariants();
    return r;
  }

  int64_t dim() const {
    return impl_->dim();
  }
  int64_t storage_offset() const {
    return impl_->storage_offset();
  }

  Tensor contiguous(MemoryFormat memory_format=MemoryFormat::Contiguous) const {
    if (is_contiguous(memory_format)) {
      return *this;
    } else {
      return __dispatch_contiguous(memory_format);
    }
  }

  Tensor conj() const {
    if (!this->is_complex()) {
      return *this;
    } else {
      if (this->is_sparse()) {
        return this->conj_physical();
      }
      return this->_conj();
    }
  }

  /// Should be used if *this can reasonably be expected to be contiguous and
  /// performance is important.
  /// Compared to contiguous, it saves a reference count
  /// increment/decrement if *this is already contiguous, at the cost
  /// in all cases of an extra pointer of stack usage, an extra branch
  /// to access, and an extra branch at destruction time.
  c10::MaybeOwned<Tensor> expect_contiguous(MemoryFormat memory_format=MemoryFormat::Contiguous) const &;

  // Use .contiguous() instead. Trying to borrow from a prvalue Tensor
  // will only lead to trouble and dangling references.
  c10::MaybeOwned<Tensor> expect_contiguous(MemoryFormat memory_format=MemoryFormat::Contiguous) && = delete;

  bool is_complex() const {
    return at::isComplexType(this->scalar_type());
  }

  bool is_floating_point() const {
    return at::isFloatingType(this->scalar_type());
  }

  bool is_signed() const {
    return at::isSignedType(this->scalar_type());
  }

  int64_t size(int64_t dim) const {
    // false is passed to maybe_wrap_dim so behavior is identical to array access (but with wrapping)
    dim = c10::maybe_wrap_dim(dim, this->dim(), false);
    return sizes()[dim];
  }

  int64_t stride(int64_t dim) const {
    // false is passed to maybe_wrap_dim so behavior is identical to array access (but with wrapping)
    dim = c10::maybe_wrap_dim(dim, this->dim(), false);
    return strides()[dim];
  }

  TensorImpl * unsafeGetTensorImpl() const {
    return impl_.get();
  }
  TensorImpl * unsafeReleaseTensorImpl() {
    return impl_.release();
  }
  const c10::intrusive_ptr<TensorImpl, UndefinedTensorImpl>& getIntrusivePtr() const {
    return impl_;
  }

  c10::intrusive_ptr<TensorImpl, UndefinedTensorImpl> unsafeReleaseIntrusivePtr()  {
    return std::move(impl_);
  }

  bool defined() const {
    return impl_;
  }

  void reset() {
    impl_.reset();
  }

  // The following overloads are very intruiging.  Consider the following
  // program:
  //
  //    x[1] = 3;
  //
  // We would expect that the first entry of x is written to 3.  But how can we
  // actually achieve this?  x[1] evaluates to a tensor...
  //
  // The answer is, using a ref-qualifier.  x[1] is an rvalue, which cannot be
  // (profitably) assigned to in the traditional sense, so we overload
  // assignment to mean, "Actually, copy 3 into the tensor data."  This is done
  // with an rvalue-reference ref-qualified overload (the methods with && at the
  // end of their type.)
  //
  // There's one more fly in the ointment: We also want
  //
  //    Tensor x = y;
  //
  // to work, and we want it NOT to copy.  So we need a traditional operator=
  // overload.  But we MUST specify a mutable lvalue ref-qualifier, to
  // disambiguate the traditional overload from the rvalue-reference
  // ref-qualified overload.  Otherwise, it will be ambiguous, because
  // a non ref-qualified method is eligible for all situations.

  // Unfortunately, we have to write these constructors out manually
  // to work around an MSVC bug:
  //    error C2580: 'at::Tensor &at::Tensor::operator =(const at::Tensor &) &':
  //    multiple versions of a defaulted special member functions are not allowed
  // Tensor& operator=(const Tensor&) & = default;
  // Tensor& operator=(Tensor&&) & = default;

  // Also MSVC will wrongly issue the following warning with the aforementioned fix
  //    warning C4522: 'at::Tensor': multiple assignment operators specified
  // Let's just skip the warning.

  #ifdef _MSC_VER
  #pragma warning( push )
  #pragma warning( disable : 4522 )
  #endif

  Tensor& operator=(const Tensor& x) & {
    impl_ = x.impl_;
    return *this;
  }
  Tensor& operator=(Tensor&& x) & {
    impl_ = std::move(x.impl_);
    return *this;
  }

  Tensor& operator=(Scalar v) &&;
  Tensor& operator=(const Tensor&) &&;
  Tensor& operator=(Tensor&&) &&;

  bool is_same(const Tensor& other) const noexcept {
    return impl_ == other.impl_;
  }
  size_t use_count() const noexcept {
    return impl_.use_count();
  }
  size_t weak_use_count() const noexcept {
    return impl_.weak_use_count();
  }

  std::string toString() const;

  IntArrayRef sizes() const {
    return impl_->sizes();
  }
  IntArrayRef strides() const {
    return impl_->strides();
  }
  // See impl::get_opt_names in ATen/NamedTensor.h for docs.
  c10::optional<DimnameList> opt_names() const {
    return impl::get_opt_names(unsafeGetTensorImpl());
  }
  // See impl::get_names in ATen/NamedTensor.h for docs.
  DimnameList names() const {
    return impl::get_names(unsafeGetTensorImpl());
  }
  int64_t ndimension() const {
    return dim();
  }

  bool is_contiguous(at::MemoryFormat memory_format=at::MemoryFormat::Contiguous) const {
    return impl_->is_contiguous(memory_format);
  }

  bool is_non_overlapping_and_dense() const {
    return impl_->is_non_overlapping_and_dense();
  }

  at::MemoryFormat suggest_memory_format(
      bool channels_last_strides_exact_match = false) const {
    // Setting channels_last_strides_exact_match to true forces function to
    // check 0,1 - sized dimension strides.
    if (!is_mkldnn() && !is_sparse()) {
      if (impl_->is_strides_like_channels_last()) {
        if (!channels_last_strides_exact_match ||
            get_channels_last_strides_2d(sizes()) == strides()) {
          return at::MemoryFormat::ChannelsLast;
        }
      }
      else if (impl_->is_strides_like_channels_last_3d()) {
        if (!channels_last_strides_exact_match ||
            get_channels_last_strides_3d(sizes()) == strides()) {
          return at::MemoryFormat::ChannelsLast3d;
        }
      }
    }
    return at::MemoryFormat::Contiguous;
  }

  // Total bytes consumed by the "view" of elements of the array.  Does not
  // include size of metadata.  The number reported here does not necessarily
  // correspond to the true physical memory consumed by a tensor; instead,
  // it reports the memory the tensor would take *if* it were contiguous.
  // Defined to be numel() * itemsize()
  size_t nbytes() const {
    TORCH_CHECK(layout () != at::kSparse,
                "nbytes is not defined for sparse tensors.  If you want the size of the constituent " \
                "tensors, add the nbytes of the indices and values.  If you want the size of the  " \
                "equivalent dense tensor, multiply numel() by element_size()");
    return impl_->numel() * impl_->itemsize();
  }

  int64_t numel() const {
    return impl_->numel();
  }

  // Length of one array element in bytes.  This is the traditional
  // Numpy naming.
  size_t itemsize() const {
    return impl_->itemsize();
  }

  // Same as itemsize().  This is the PyTorch naming.
  int64_t element_size() const {
    return static_cast<int64_t>(impl_->itemsize());
  }

  C10_DEPRECATED_MESSAGE("Tensor.type() is deprecated. Instead use Tensor.options(), which in many cases (e.g. in a constructor) is a drop-in replacement. If you were using data from type(), that is now available from Tensor itself, so instead of tensor.type().scalar_type(), use tensor.scalar_type() instead and instead of tensor.type().backend() use tensor.device().")
  DeprecatedTypeProperties & type() const {
    return globalDeprecatedTypePropertiesRegistry().getDeprecatedTypeProperties(
        dispatchKeyToBackend(legacyExtractDispatchKey(key_set())),
        scalar_type());
  }
  DispatchKeySet key_set() const {
    return impl_->key_set();
  }
  ScalarType scalar_type() const {
    return typeMetaToScalarType(impl_->dtype());
  }
  bool has_storage() const {
    return defined() && impl_->has_storage();
  }
  const Storage& storage() const {
    return impl_->storage();
  }
  bool is_alias_of(const at::Tensor& other) const{
    return impl_->storage().is_alias_of(other.storage());
  }
  Tensor toType(ScalarType t) const;
  Tensor toBackend(Backend b) const;

  C10_DEPRECATED_MESSAGE("Tensor.is_variable() is deprecated; everything is a variable now. (If you want to assert that variable has been appropriately handled already, use at::impl::variable_excluded_from_dispatch())")
  bool is_variable() const noexcept {
    return !at::impl::variable_excluded_from_dispatch();
  }

  inline bool is_conj() const {
    return impl_->is_conj();
  }

  // sets the conjugate bit of a tensor.
  // NOTE: Conjugate bit is supposed to be a read-only field. Only change this, if you are extremely sure
  // that's what you want. Changing this might lead to incorrect behavior since conjugation is
  // a lazy operation and we rely on this bit to determine if a conjugation needs to be materialized.
  inline void _set_conj(bool conjugate) const {
    impl_->_set_conj(conjugate);
  }

  /// Returns a `Tensor`'s layout.
  Layout layout() const noexcept {
    return impl_->layout();
  }

  /// Returns a `Tensor`'s dtype (`TypeMeta`).
  caffe2::TypeMeta dtype() const noexcept {
    return impl_->dtype();
  }

  /// Returns a `Tensor`'s device.
  inline Device device() const {
    return impl_->device();
  }

  /// Returns a `Tensor`'s device index.
  int64_t get_device() const {
    // NB: this is not a native function to avoid dispatching overhead.
    return impl_->get_device();
  }


  /// Returns if a `Tensor` has CPU backend.
  bool is_cpu() const {
    // NB: this is not a native function to avoid dispatching overhead.
    return impl_->is_cpu();
  }

  /// Returns if a `Tensor` has CUDA backend.
  bool is_cuda() const {
    // NB: this is not a native function to avoid dispatching overhead.
    return impl_->is_cuda();
  }

  /// Returns if a `Tensor` has XPU backend.
  bool is_xpu() const {
    // NB: this is not a native function to avoid dispatching overhead.
    return impl_->is_xpu();
  }

  /// Returns if a `Tensor` has XLA backend.
  bool is_xla() const {
    return impl_->is_xla();
  }

  /// Returns if a `Tensor` has HIP backend.
  bool is_hip() const {
    // NB: this is not a native function to avoid dispatching overhead.
    return impl_->is_hip();
  }

  /// Returns if a `Tensor` has sparse backend.
  bool is_sparse() const {
    // NB: this is not a native function to avoid dispatching overhead.
    return impl_->is_sparse();
  }

  /// Returns is a `Tensor` has a sparse CSR backend.
  bool is_sparse_csr() const {
    // NB: this is not a native function to avoid dispatching overhead.
    return impl_->is_sparse_csr();
  }

  /// Returns if a `Tensor` is mkldnn tensor.
  bool is_mkldnn() const {
    // NB: this is not a native function to avoid dispatching overhead.
    return impl_->is_mkldnn();
  }

  /// Returns if a `Tensor` is mlc tensor.
  bool is_mlc() const {
    // NB: this is not a native function to avoid dispatching overhead.
    return impl_->is_mlc();
  }

  /// Returns if a `Tensor` is vulkan tensor.
  bool is_vulkan() const {
    // NB: this is not a native function to avoid dispatching overhead.
    return impl_->is_vulkan();
  }

  /// Returns if a `Tensor` is metal tensor.
  bool is_metal() const {
    // NB: this is not a native function to avoid dispatching overhead.
    return impl_->is_metal();
  }

  /// Returns if a `Tensor` has quantized backend.
  bool is_quantized() const {
    // NB: this is not a native function to avoid dispatching overhead.
    return impl_->is_quantized();
  }

  /// Returns if a `Tensor` is a meta tensor.  Meta tensors can
  /// also have other designations.
  bool is_meta() const {
    return impl_->is_meta();
  }

  /// Returns if a `Tensor` is an inference tensor.
  bool is_inference() const {
    return impl_->is_inference();
  }

  /// If a tensor is a quantized tensor, returns its quantizer
  /// TODO: it's not in native_functions.yaml yet as it's not exposed to python
  QuantizerPtr quantizer() const;

  /// Returns if a `Tensor` has any dimension names
  bool has_names() const {
    // If a user is using unnamed tensors, then we can short-circuit right here.
    // Otherwise, impl::has_names attempts to retrieve names.
    if (!impl_->has_named_tensor_meta()) {
      return false;
    }
    return impl::has_names(unsafeGetTensorImpl());
  }

  /// Returns a `Tensor`'s dimension names data structure
  const NamedTensorMeta* get_named_tensor_meta() const {
    return static_cast<NamedTensorMeta*>(impl_->named_tensor_meta());
  }

  NamedTensorMeta* get_named_tensor_meta() {
    return static_cast<NamedTensorMeta*>(impl_->named_tensor_meta());
  }

  /// Returns the `TensorOptions` corresponding to this `Tensor`. Defined in
  /// TensorOptions.h.
  TensorOptions options() const;

  void* data_ptr() const {
    return this->unsafeGetTensorImpl()->data();
  }

  template <typename T>
  T * data_ptr() const;

  template<typename T>
  C10_DEPRECATED_MESSAGE("Tensor.data<T>() is deprecated. Please use Tensor.data_ptr<T>() instead.")
  T * data() const {
    return data_ptr<T>();
  }

  template <typename T>
  T item() const;

  // Purposely not defined here to avoid inlining
  void print() const;

  // Return a `TensorAccessor` for CPU `Tensor`s. You have to specify scalar type and
  // dimension.
  template<typename T, size_t N>
  TensorAccessor<T,N> accessor() const& {
    static_assert(N > 0, "accessor is used for indexing tensor, for scalars use *data_ptr<T>()");
    TORCH_CHECK(dim() == N, "TensorAccessor expected ", N, " dims but tensor has ", dim());
    return TensorAccessor<T,N>(data_ptr<T>(),sizes().data(),strides().data());
  }
  template<typename T, size_t N>
  TensorAccessor<T,N> accessor() && = delete;

  // Return a `GenericPackedTensorAccessor` for CUDA `Tensor`s. You have to specify scalar type and
  // dimension. You can optionally specify RestrictPtrTraits as a template parameter to
  // cast the data pointer to a __restrict__ pointer.
  // In order to use this, your CUDA kernel has to take a corresponding GenericPackedTensorAccessor
  // as an argument.
  template<typename T, size_t N, template <typename U> class PtrTraits = DefaultPtrTraits, typename index_t = int64_t>
  GenericPackedTensorAccessor<T,N,PtrTraits,index_t> generic_packed_accessor() const& {
    static_assert(N > 0, "accessor is used for indexing tensor, for scalars use *data_ptr<T>()");
    TORCH_CHECK(dim() == N, "TensorAccessor expected ", N, " dims but tensor has ", dim());
    return GenericPackedTensorAccessor<T,N,PtrTraits,index_t>(static_cast<typename PtrTraits<T>::PtrType>(data_ptr<T>()),sizes().data(),strides().data());
  }
  template<typename T, size_t N, template <typename U> class PtrTraits = DefaultPtrTraits, typename index_t = int64_t>
  GenericPackedTensorAccessor<T,N> generic_packed_accessor() && = delete;

  template<typename T, size_t N, template <typename U> class PtrTraits = DefaultPtrTraits>
  PackedTensorAccessor32<T,N,PtrTraits> packed_accessor32() const& {
    return generic_packed_accessor<T,N,PtrTraits,int32_t>();
  }
  template<typename T, size_t N, template <typename U> class PtrTraits = DefaultPtrTraits>
  PackedTensorAccessor32<T,N,PtrTraits> packed_accessor32() && = delete;

  template<typename T, size_t N, template <typename U> class PtrTraits = DefaultPtrTraits>
  PackedTensorAccessor64<T,N,PtrTraits> packed_accessor64() const& {
    return generic_packed_accessor<T,N,PtrTraits,int64_t>();
  }
  template<typename T, size_t N, template <typename U> class PtrTraits = DefaultPtrTraits>
  PackedTensorAccessor64<T,N,PtrTraits> packed_accessor64() && = delete;

  template<typename T, size_t N, template <typename U> class PtrTraits = DefaultPtrTraits, typename index_t = int64_t>
  C10_DEPRECATED_MESSAGE("packed_accessor is deprecated, use packed_accessor32 or packed_accessor64 instead")
  GenericPackedTensorAccessor<T,N,PtrTraits,index_t> packed_accessor() const & {
    return generic_packed_accessor<T,N,PtrTraits,index_t>();
  }
  template<typename T, size_t N, template <typename U> class PtrTraits = DefaultPtrTraits, typename index_t = int64_t>
  C10_DEPRECATED_MESSAGE("packed_accessor is deprecated, use packed_accessor32 or packed_accessor64 instead")
  GenericPackedTensorAccessor<T,N,PtrTraits,index_t> packed_accessor() && = delete;

  Tensor operator~() const;
  Tensor operator-() const;
  Tensor& operator+=(const Tensor & other);
  Tensor& operator+=(Scalar other);
  Tensor& operator-=(const Tensor & other);
  Tensor& operator-=(Scalar other);
  Tensor& operator*=(const Tensor & other);
  Tensor& operator*=(Scalar other);
  Tensor& operator/=(const Tensor & other);
  Tensor& operator/=(Scalar other);
  Tensor& operator&=(const Tensor & other);
  Tensor& operator|=(const Tensor & other);
  Tensor& operator^=(const Tensor & other);
  Tensor operator[](Scalar index) const;
  Tensor operator[](Tensor index) const;
  Tensor operator[](int64_t index) const;

  Tensor index(ArrayRef<at::indexing::TensorIndex> indices) const;
  Tensor index(std::initializer_list<at::indexing::TensorIndex> indices) const;

  Tensor & index_put_(ArrayRef<at::indexing::TensorIndex> indices, Tensor const & rhs);
  Tensor & index_put_(ArrayRef<at::indexing::TensorIndex> indices, const Scalar& v);
  Tensor & index_put_(std::initializer_list<at::indexing::TensorIndex> indices, Tensor const & rhs);
  Tensor & index_put_(std::initializer_list<at::indexing::TensorIndex> indices, const Scalar& v);

  Tensor cpu() const;
  Tensor cuda() const;
  Tensor hip() const;
  Tensor vulkan() const;
  Tensor metal() const;

  // ~~~~~ Autograd API ~~~~~

  /// \fn bool is_leaf() const;
  ///
  /// All Tensors that have `requires_grad()` which is ``false`` will be leaf Tensors by convention.
  ///
  /// For Tensors that have `requires_grad()` which is ``true``, they will be leaf Tensors if they were
  /// created by the user. This means that they are not the result of an operation and so
  /// `grad_fn()` is `nullptr`.
  ///
  /// Only leaf Tensors will have their `grad()` populated during a call to `backward()`.
  /// To get `grad()` populated for non-leaf Tensors, you can use `retain_grad()`.
  ///
  /// Example:
  /// @code
  /// auto a = torch::rand(10, torch::requires_grad());
  /// std::cout << a.is_leaf() << std::endl; // prints `true`
  ///
  /// auto b = torch::rand(10, torch::requires_grad()).to(torch::kCUDA);
  /// std::cout << b.is_leaf() << std::endl; // prints `false`
  /// // b was created by the operation that cast a cpu Tensor into a cuda Tensor
  ///
  /// auto c = torch::rand(10, torch::requires_grad()) + 2;
  /// std::cout << c.is_leaf() << std::endl; // prints `false`
  /// // c was created by the addition operation
  ///
  /// auto d = torch::rand(10).cuda();
  /// std::cout << d.is_leaf() << std::endl; // prints `true`
  /// // d does not require gradients and so has no operation creating it (that is tracked by the autograd engine)
  ///
  /// auto e = torch::rand(10).cuda().requires_grad_();
  /// std::cout << e.is_leaf() << std::endl; // prints `true`
  /// // e requires gradients and has no operations creating it
  ///
  /// auto f = torch::rand(10, torch::device(torch::kCUDA).requires_grad(true));
  /// std::cout << f.is_leaf() << std::endl; // prints `true`
  /// // f requires grad, has no operation creating it
  /// @endcode

  /// \fn void backward(const Tensor & gradient={}, c10::optional<bool> retain_graph=c10::nullopt, bool create_graph=false, c10::optional<TensorList> inputs=c10::nullopt) const;
  ///
  /// Computes the gradient of current tensor with respect to graph leaves.
  ///
  /// The graph is differentiated using the chain rule. If the tensor is
  /// non-scalar (i.e. its data has more than one element) and requires
  /// gradient, the function additionally requires specifying ``gradient``.
  /// It should be a tensor of matching type and location, that contains
  /// the gradient of the differentiated function w.r.t. this Tensor.
  ///
  /// This function accumulates gradients in the leaves - you might need to
  /// zero them before calling it.
  ///
  /// \param gradient Gradient w.r.t. the
  ///     tensor. If it is a tensor, it will be automatically converted
  ///     to a Tensor that does not require grad unless ``create_graph`` is True.
  ///     None values can be specified for scalar Tensors or ones that
  ///     don't require grad. If a None value would be acceptable then
  ///     this argument is optional.
  /// \param retain_graph If ``false``, the graph used to compute
  ///     the grads will be freed. Note that in nearly all cases setting
  ///     this option to True is not needed and often can be worked around
  ///     in a much more efficient way. Defaults to the value of
  ///     ``create_graph``.
  /// \param create_graph If ``true``, graph of the derivative will
  ///     be constructed, allowing to compute higher order derivative
  ///     products. Defaults to ``false``.
  /// \param inputs Inputs w.r.t. which the gradient will be accumulated into
  ///     ``at::Tensor::grad``. All other Tensors will be ignored. If not
  ///     provided, the gradient is accumulated into all the leaf Tensors
  ///     that were used to compute the current tensor. All the provided inputs
  ///     must be leaf Tensors.
  void backward(const Tensor & gradient={}, c10::optional<bool> retain_graph=c10::nullopt, bool create_graph=false, c10::optional<TensorList> inputs=c10::nullopt) const {
    // NB: Adding this wrapper to _backward here because we'd like our
    // 'backwards' api to accept the 'inputs' argument optionally. Since code gen
    // currently does not support optional of TensorList our approach is to replace
    // backward in native_functions.yaml with _backward and call it here instead.
    if (inputs.has_value()) {
      TORCH_CHECK(inputs.value().size() > 0, "'inputs' argument to backward cannot be empty")
      this->_backward(inputs.value(), gradient, retain_graph, create_graph);
    } else {
      this->_backward({}, gradient, retain_graph, create_graph);
    }
  }

  /// \fn Tensor detach() const;
  ///
  /// Returns a new Tensor, detached from the current graph.
  /// The result will never require gradient.

  /// \fn Tensor & detach_() const;
  ///
  /// Detaches the Tensor from the graph that created it, making it a leaf.
  /// Views cannot be detached in-place.

  /// \fn void retain_grad() const;
  ///
  /// Enables .grad() for non-leaf Tensors.

  const Tensor& set_requires_grad(bool requires_grad) const {
    impl_->set_requires_grad(requires_grad);
    return *this;
  }
  bool requires_grad() const {
    return impl_->requires_grad();
  }

  /// Return a mutable reference to the gradient. This is conventionally
  /// used as `t.grad() = x` to set a gradient to a completely new tensor.
  /// Note that this function work with a non-const Tensor and is not
  /// thread safe.
  Tensor& mutable_grad() const {
    return impl_->mutable_grad();
  }

  /// This function returns an undefined tensor by default and returns a defined tensor
  /// the first time a call to `backward()` computes gradients for this Tensor.
  /// The attribute will then contain the gradients computed and future calls
  /// to `backward()` will accumulate (add) gradients into it.
  const Tensor& grad() const {
    return impl_->grad();
  }

  // The Forward AD API functions below are low level and are not to be used by end
  // users who should use the API provided in torch/csrc/autograd.h

  /// This function returns the forward gradient for this Tensor at the given level.
  const Tensor& _fw_grad(uint64_t level) const {
    return impl_->_fw_grad(level, *this);
  }

  /// This function can be used to set the value of the forward grad.
  /// Note that the given new_grad might not be used directly if it has different
  /// metadata (size/stride/storage offset) compared to this Tensor. In that case,
  /// new_grad content will be copied into a new Tensor
  void _set_fw_grad(const Tensor& new_grad, uint64_t level, bool is_inplace_op) const {
    impl_->_set_fw_grad(new_grad, *this, level, is_inplace_op);
  }


  // STOP.  Thinking of adding a method here, which only makes use
  // of other ATen methods?  Define it in native_functions.yaml.

  //example
  //Tensor * add(Tensor & b);
  ${tensor_method_declarations}

  // Special C++ only overloads for std()-like functions (See gh-40287)
  // These are needed because int -> bool conversion takes precedence over int -> IntArrayRef
  // So, for example std(0) would select the std(unbiased=False) overload

  Tensor var(int dim) const {
    return var(IntArrayRef{dim});
  }

  Tensor std(int dim) const {
    return std(IntArrayRef{dim});
  }

  // We changed .dtype() to return a TypeMeta in #12766. Ideally, we want the
  // at::kDouble and its friends to be TypeMeta's, but that hasn't happened yet.
  // Before that change, we make this method to maintain BC for C++ usage like
  // `x.to(y.dtype)`.
  // TODO: remove following two after at::kDouble and its friends are TypeMeta's.
  inline Tensor to(caffe2::TypeMeta type_meta, bool non_blocking=false, bool copy=false) const {
    return this->to(/*scalar_type=*/typeMetaToScalarType(type_meta), non_blocking, copy);
  }
  inline Tensor to(Device device, caffe2::TypeMeta type_meta, bool non_blocking=false, bool copy=false) const {
    return this->to(device, /*scalar_type=*/typeMetaToScalarType(type_meta), non_blocking, copy);
  }

  template <typename F, typename... Args>
  decltype(auto) m(F func, Args&&... params) const {
    return func(*this, std::forward<Args>(params)...);
  }

  /// NOTE: This is similar to the legacy `.data()` function on `Variable`, and is intended
  /// to be used from functions that need to access the `Variable`'s equivalent `Tensor`
  /// (i.e. `Tensor` that shares the same storage and tensor metadata with the `Variable`).
  ///
  /// One notable difference with the legacy `.data()` function is that changes to the
  /// returned `Tensor`'s tensor metadata (e.g. sizes / strides / storage / storage_offset)
  /// will not update the original `Variable`, due to the fact that this function
  /// shallow-copies the `Variable`'s underlying TensorImpl.
  at::Tensor tensor_data() const;

  /// NOTE: `var.variable_data()` in C++ has the same semantics as `tensor.data`
  /// in Python, which create a new `Variable` that shares the same storage and
  /// tensor metadata with the original `Variable`, but with a completely new
  /// autograd history.
  ///
  /// NOTE: If we change the tensor metadata (e.g. sizes / strides /
  /// storage / storage_offset) of a variable created from `var.variable_data()`, those
  /// changes will not update the original variable `var`. In `.variable_data()`, we set
  /// `allow_tensor_metadata_change_` to false to make such changes explicitly illegal,
  /// in order to prevent users from changing metadata of `var.variable_data()`
  /// and expecting the original variable `var` to also be updated.
  at::Tensor variable_data() const;

  // Gradient Node and Edges
  //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

  /// Gets the gradient function of the `Variable`. If this is a leaf variable,
  /// the pointer returned will be null.
  ///
  /// For View Variables:
  /// Gets the up-to-date grad_fn. If the shared data or base was modified, we
  /// re-create the grad_fn to express the up-to-date view relationship between
  /// this and the base Variable.
  const std::shared_ptr<torch::autograd::Node>& grad_fn() const;

  // Hooks
  //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

  template <typename T>
  using hook_return_void_t = std::enable_if_t<std::is_void<typename std::result_of<T&(Tensor)>::type>::value, unsigned>;
  template <typename T>
  using hook_return_var_t = std::enable_if_t<std::is_same<typename std::result_of<T&(Tensor)>::type, Tensor>::value, unsigned>;

  /// Registers a backward hook.
  ///
  /// The hook will be called every time a gradient with respect to the Tensor is computed.
  /// The hook should have one of the following signature:
  /// ```
  /// hook(Tensor grad) -> Tensor
  /// ```
  /// ```
  /// hook(Tensor grad) -> void
  /// ```
  /// The hook should not modify its argument, but it can optionally return a new gradient
  /// which will be used in place of `grad`.
  ///
  /// This function returns the index of the hook in the list which can be used to remove hook.
  ///
  /// Example:
  /// @code
  /// auto v = torch::tensor({0., 0., 0.}, torch::requires_grad());
  /// auto h = v.register_hook([](torch::Tensor grad){ return grad * 2; }); // double the gradient
  /// v.backward(torch::tensor({1., 2., 3.}));
  /// // This prints:
  /// // ```
  /// //  2
  /// //  4
  /// //  6
  /// // [ CPUFloatType{3} ]
  /// // ```
  /// std::cout << v.grad() << std::endl;
  /// v.remove_hook(h);  // removes the hook
  /// @endcode
  template <typename T>
  hook_return_void_t<T> register_hook(T&& hook) const;
  template <typename T>
  hook_return_var_t<T> register_hook(T&& hook) const;

private:
  unsigned _register_hook(std::function<Tensor(const Tensor&)> hook) const;

public:

  /// Remove hook at given position
  void remove_hook(unsigned pos) const;

  // Variable methods
  //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

  bool is_leaf() const;

  int64_t output_nr() const;

  void set_data(const Tensor & new_data) const;

  Tensor data() const;

  int64_t _version() const;

  void retain_grad() const;

  void _backward(TensorList inputs, const c10::optional<Tensor>& gradient, c10::optional<bool> keep_graph, bool create_graph) const;

  const Tensor& requires_grad_(bool _requires_grad=true) const;

  // View Variables
  //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

  /// Returns true if this `Variable` is a view of another `Variable`.
  bool is_view() const;

  /// Returns the `Variable` that this `Variable` is a view of. If this
  /// `Variable` is not a view, throw a `std::runtime_error`.
  const Tensor& _base() const;

  // Miscellaneous
  //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

  const std::string& name() const;

protected:
  friend class ::caffe2::Tensor;

  void enforce_invariants();
  c10::intrusive_ptr<TensorImpl, UndefinedTensorImpl> impl_;
};

// For "multiple ... operators specified" warnings, closing brace of class
// declaration must be included between pragma push & pop
#ifdef _MSC_VER
#pragma warning( pop )
#endif

inline int64_t get_device(const Tensor& self) {
  return self.get_device();
}

template <typename T>
auto Tensor::register_hook(T&& hook) const -> Tensor::hook_return_void_t<T> {
  // Return the grad argument in case of a hook with void return type to have an
  // std::function with Tensor return type
  static_assert(std::is_same<decltype(hook(Tensor())), void>::value,
                "Expected hook to return void");
  return _register_hook([fn=std::forward<T>(hook)](const Tensor& grad) {
    fn(grad);
    return Tensor();
  });
}

template <typename T>
auto Tensor::register_hook(T&& hook) const -> Tensor::hook_return_var_t<T> {
  return _register_hook(std::forward<T>(hook));
}

namespace detail {
// Helper creator for Tensor class which doesn't requires the users to pass
// in an intrusive_ptr instead it just converts the argument passed to
// requested intrusive_ptr type.
template <typename T, typename... Args>
Tensor make_tensor(Args&&... args) {
  return Tensor(c10::make_intrusive<T>(std::forward<Args>(args)...));
}

} // namespace detail

static inline DispatchKey legacyExtractDispatchKey(const Tensor& t) {
  return legacyExtractDispatchKey(t.key_set());
}

} // namespace at

namespace c10 {
template <>
struct MaybeOwnedTraits<at::Tensor> {
  using owned_type = at::Tensor;
  using borrow_type = at::Tensor;

  static borrow_type createBorrow(const owned_type& from) {
    // NOTE: this can be implemented without the special
    // unsafe_borrow_t Tensor constructor as
    //
    // return borrow_type(c10::intrusive_ptr<at::TensorImpl, at::UndefinedTensorImpl>::reclaim(from.unsafeGetTensorImpl()));
    //
    // but that hurts inlining due to the nullptr check in the
    // Tensor(c10::intrusive_ptr<...>) constructor. We already know
    // that from.impl_ isn't null because from is a valid Tensor, so
    // we needn't do the check again. (using __builtin_assume can
    // avoid this, but wouldn't be portable to MSVC.)
    return borrow_type(borrow_type::unsafe_borrow_t{}, from);
  }

  static void assignBorrow(borrow_type& lhs, const borrow_type& rhs) {
    lhs.unsafeReleaseTensorImpl();
    // See above note: this can be implemented with public API
    // similarly to createBorrow(), but that would hurt inlining.
    lhs = borrow_type(borrow_type::unsafe_borrow_t{}, rhs);
  }

  static void destroyBorrow(borrow_type& toDestroy) {
    toDestroy.unsafeReleaseTensorImpl(); // "leak" it, but it was already +0.
  }

  static const owned_type& referenceFromBorrow(const borrow_type& borrow) {
    return borrow;
  }

  static const owned_type* pointerFromBorrow(const borrow_type& borrow) {
    return &borrow;
  }

  static bool debugBorrowIsValid(const borrow_type& borrow) {
    return true;
  }
};
} // namespace c10

namespace at {

inline c10::MaybeOwned<Tensor> borrow_from_optional_tensor(
    const c10::optional<Tensor>& opt) {
  return opt.has_value()
    ? c10::MaybeOwned<Tensor>::borrowed(*opt)
    : c10::MaybeOwned<Tensor>::owned(c10::in_place);
}

inline c10::MaybeOwned<Tensor> Tensor::expect_contiguous(MemoryFormat memory_format) const & {
  if (is_contiguous(memory_format)) {
    return c10::MaybeOwned<Tensor>::borrowed(*this);
  } else {
    return c10::MaybeOwned<Tensor>::owned(__dispatch_contiguous(memory_format));
  }
}
} // namespace at
