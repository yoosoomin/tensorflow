/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#ifndef TENSORFLOW_LITE_MICRO_MICRO_ALLOCATOR_H_
#define TENSORFLOW_LITE_MICRO_MICRO_ALLOCATOR_H_

#include <cstddef>
#include <cstdint>

#include "flatbuffers/flatbuffers.h"  // from @flatbuffers
#include "tensorflow/lite/c/common.h"
#include "tensorflow/lite/core/api/error_reporter.h"
#include "tensorflow/lite/micro/compatibility.h"
#include "tensorflow/lite/micro/micro_op_resolver.h"
#include "tensorflow/lite/micro/simple_memory_allocator.h"
#include "tensorflow/lite/schema/schema_generated.h"

namespace tflite {

// Namespace used for unittests.
namespace internal {

// Sets up all of the data structure members for a TfLiteTensor based on the
// contents of a serialized tensor in the flatbuffer.
TfLiteStatus InitializeTfLiteTensorFromFlatbuffer(
    SimpleMemoryAllocator* allocator, const tflite::Tensor& flatbuffer_tensor,
    const flatbuffers::Vector<flatbuffers::Offset<Buffer>>* buffers,
    ErrorReporter* error_reporter, TfLiteTensor* result);

// A handle tracking scratch buffer allocation. This handle is created by
// `RequestScratchBufferInArena`. `data` field is populated in
// `FinishTensorAllocation` after static memory planning.
// TODO(b/150257460) As a future optimization, this struct could be replaced by
// a union, since once `data` is populated, `bytes` and `node_idx` is not
// needed.
typedef struct {
  // Pointer to the scratch buffer.
  uint8_t* data;
  // Number of bytes required by the buffer. The actual allocated size might be
  // greater than `bytes` due to buffer alignment.
  size_t bytes;
  // Node where the buffer is allocated for. This provides useful information to
  // determine the lifetime of the buffer. In AllocationInfo, this buffer will
  // have `before` = node_idx and `after` = node_idx.
  int node_idx;
} ScratchBufferHandle;
}  // namespace internal

typedef struct {
  TfLiteNode node;
  const TfLiteRegistration* registration;
} NodeAndRegistration;

// Allocator responsible for allocating memory for all intermediate tensors
// necessary to invoke a model.

// Memory layout to help understand how it works
// This information could change in the future version.
// ************** .memory_allocator->GetBuffer()
// Tensors/Scratch buffers (head)
// ************** .head_watermark
// unused memory
// ************** .memory_allocator->GetBuffer() + ->GetMaxBufferSize()
//                                               - ->GetDataSize()
// persistent area (tail)
// ************** .memory_allocator->GetBuffer() + ->GetMaxBufferSize()
class MicroAllocator {
 public:
  // The lifetime of the model, tensor allocator and error reporter must be at
  // least as long as that of the allocator object, since the allocator needs
  // them to be accessible during its entire lifetime.

  // Note: Please use __declspec(align(16)) to make sure tensor_arena is 16
  // bytes aligned, otherwise some head room will be wasted.
  MicroAllocator(TfLiteContext* context, const Model* model,
                 uint8_t* tensor_arena, size_t arena_size,
                 ErrorReporter* error_reporter);
  virtual ~MicroAllocator();

  // Initializes the allocator by allocating required internal structs required
  // to prepare the model from the flatbuffer data in PrepareFromFlatbuffer.
  TfLiteStatus Init();

  // Run through the model flatbuffer data (loaded from the TfLiteModel
  // instance) to allocate nodes and registrations. We need to keep them for the
  // entire life time of the model to allow persistent tensors. This method
  // needs to be called before FinishTensorAllocation method. This method also
  // allocates any internal Op data that is required from the flatbuffer.
  TfLiteStatus PrepareFromFlatbuffer(
      const MicroOpResolver& op_resolver,
      NodeAndRegistration** node_and_registrations);

  // Runs through the model and allocates all necessary input, output and
  // intermediate tensors.
  // WARNING: doing any allocation after calling this method has the risk of
  // corrupting tensor data so this method should be the last non-const method
  // called in this class.
  TfLiteStatus FinishTensorAllocation();

  // Allocates persistent buffer which has the same life time as the allocator.
  // The memory is immediately available and is allocated from the tail of the
  // arena.
  TfLiteStatus AllocatePersistentBuffer(size_t bytes, void** ptr);

  // Register a scratch buffer of size `bytes` for Node with `node_id`.
  // This method only allocates a BufferHandle holding information for memory
  // planning. The buffer ptr is ready after `FinishTensorAllocation` and can
  // be retrieved by `GetScratchBuffer` method using the returned buffer_idx.
  // Note that there should be no tail allocation between two consecutive
  // `RequestScratchBufferInArena` calls.
  TfLiteStatus RequestScratchBufferInArena(int node_id, size_t bytes,
                                           int* buffer_idx);
  // Returns the pointer to the planned scratch buffer.
  void* GetScratchBuffer(int buffer_idx) const;

  // Returns the arena usage in bytes, only available after
  // `FinishTensorAllocation`. Otherwise, it will return 0.
  size_t used_bytes() const;

 protected:
  MicroAllocator(TfLiteContext* context, const Model* model,
                 SimpleMemoryAllocator* memory_allocator,
                 ErrorReporter* error_reporter);

  // Allocates an array in the arena to hold pointers to the tensors required
  // to initialize and prepare a model. These allocations are stored and
  // populated on the context.
  virtual TfLiteStatus AllocateTfLiteTensorArray();

  // Populates content on the list of tensor pointers required to initialize and
  // prepare a model from data in the flatbuffer (loaded from the TfLiteModel
  // instance). Persistent data (e.g. quantization params) is allocated from the
  // arena.
  virtual TfLiteStatus PopulateTfLiteTensorArrayFromFlatbuffer();

  // Allocates an array in the arena to hold pointers to the node and
  // registration pointers required to represent the inference graph of the
  // model.
  virtual TfLiteStatus AllocateNodeAndRegistrations(
      NodeAndRegistration** node_and_registrations);

  // Populates node and registration pointers representing the inference graph
  // of the model from values inside the flatbuffer (loaded from the TfLiteModel
  // instance). Persistent data (e.g. operator data) is allocated from the
  // arena.
  virtual TfLiteStatus PrepareNodeAndRegistrationDataFromFlatbuffer(
      const MicroOpResolver& op_resolver,
      NodeAndRegistration* node_and_registrations);

  // Returns the number of tensors in the model subgraph.
  size_t GetTensorsCount() const;

  // Returns the number of operators in the model subgraph.
  size_t GetOperatorsCount() const;

  ErrorReporter* error_reporter();

 private:
  TfLiteStatus InitGraphAndContextTensorData();

  // A simple memory allocator that always allocate from the arena tail.
  SimpleMemoryAllocator* memory_allocator_;

  const Model* model_;
  TfLiteContext* context_;
  ErrorReporter* error_reporter_;
  // Indicating if the allocator is ready for allocation.
  bool active_ = false;

  // In reverse order for efficiency.
  // i.e. scratch_buffer_handles_[0] is the handle for the last buffer,
  // corresponding to the last RequestScratchBufferInArena call.
  internal::ScratchBufferHandle* scratch_buffer_handles_ = nullptr;
  // How many scratch buffers have been allocated.
  size_t scratch_buffer_count_ = 0;

  const SubGraph* subgraph_;

  TF_LITE_REMOVE_VIRTUAL_DELETE
};

}  // namespace tflite
#endif  // TENSORFLOW_LITE_MICRO_MICRO_ALLOCATOR_H_
