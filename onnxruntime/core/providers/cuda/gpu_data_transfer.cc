// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/providers/cuda/gpu_data_transfer.h"
#include "cuda_common.h"

// use default stream for copy for now, to avoid racing in BFC arena as in issue #4829
// note this may cause some models to run slower if there are ops running on CPU
// so we leave it as optional, in case user need the previous behavior
// a full fix to BFC arena is being looked at, and once it's in, we can revert this change
namespace onnxruntime {
GPUDataTransfer::GPUDataTransfer(bool do_copy_in_default_stream) {
  // create streams, default is nullptr
  streams_[kCudaStreamDefault] = nullptr;
  if (do_copy_in_default_stream) {
    streams_[kCudaStreamCopyIn] = nullptr;
    streams_[kCudaStreamCopyOut] = nullptr;
  } else {
    CUDA_CALL_THROW(cudaStreamCreateWithFlags(&streams_[kCudaStreamCopyIn], cudaStreamNonBlocking));
    CUDA_CALL_THROW(cudaStreamCreateWithFlags(&streams_[kCudaStreamCopyOut], cudaStreamNonBlocking));
  }
}

GPUDataTransfer::~GPUDataTransfer() {
  if (streams_[kCudaStreamCopyIn] != nullptr) {
    CUDA_CALL(cudaStreamDestroy(streams_[kCudaStreamCopyIn]));
  }
  if (streams_[kCudaStreamCopyOut] != nullptr) {
    CUDA_CALL(cudaStreamDestroy(streams_[kCudaStreamCopyOut]));
  }
}

bool GPUDataTransfer::CanCopy(const OrtDevice& src_device, const OrtDevice& dst_device) const {
  return src_device.Type() == OrtDevice::GPU || src_device.MemType() == OrtDevice::MemType::CUDA_PINNED ||
         dst_device.Type() == OrtDevice::GPU || dst_device.MemType() == OrtDevice::MemType::CUDA_PINNED;
}

common::Status GPUDataTransfer::CopyTensor(const Tensor& src, Tensor& dst, int exec_queue_id) const {
  size_t bytes = src.SizeInBytes();
  const void* src_data = src.DataRaw();
  void* dst_data = dst.MutableDataRaw();

  auto& src_device = src.Location().device;
  auto& dst_device = dst.Location().device;

  if (dst_device.Type() == OrtDevice::GPU) {
    if (src_device.Type() == OrtDevice::CPU && src_device.MemType() == OrtDevice::MemType::CUDA_PINNED) {
      // copy from pinned memory to GPU, this is non-blocking
      std::cout << "copying from pinned memory to device " << dst_device.Id() << std::endl;
      CUDA_RETURN_IF_ERROR(cudaMemcpyAsync(dst_data, src_data, bytes, cudaMemcpyHostToDevice, streams_[exec_queue_id]));
    } else if (src_device.Type() == OrtDevice::GPU) {
      // copying between GPU, this is non-blocking
      // Copy only if the two addresses are different.
      if (dst_data != src_data) {
        std::cout << "copying from device " << src_device.Id() << " to device " << dst_device.Id() << std::endl;
        CUDA_RETURN_IF_ERROR(cudaMemcpyAsync(dst_data, src_data, bytes, cudaMemcpyDeviceToDevice, streams_[kCudaStreamDefault]));
      }
    } else {
      // copy from other CPU memory to GPU, this is blocking
      std::cout << "copying from CPU to device " << dst_device.Id() << std::endl;
      CUDA_RETURN_IF_ERROR(cudaMemcpy(dst_data, src_data, bytes, cudaMemcpyHostToDevice));
    }
  } else if (src_device.Type() == OrtDevice::GPU) {
    if (dst_device.Type() == OrtDevice::CPU && dst_device.MemType() == OrtDevice::MemType::CUDA_PINNED) {
      // copying from GPU to pinned memory, this is non-blocking
      std::cout << "copying from device " << src_device.Id() << " to pinned memory" << std::endl;
      CUDA_RETURN_IF_ERROR(cudaMemcpyAsync(dst_data, src_data, bytes, cudaMemcpyDeviceToHost, streams_[exec_queue_id]));
    } else {
      // copying from GPU to CPU memory, this is blocking
      std::cout << "copying from device " << src_device.Id() << " to CPU" << std::endl;
      CUDA_RETURN_IF_ERROR(cudaMemcpy(dst_data, src_data, bytes, cudaMemcpyDeviceToHost));
    }
  } else {
    // copying between cpu memory
    memcpy(dst_data, src_data, bytes);
  }

  return Status::OK();
}
}  // namespace onnxruntime
