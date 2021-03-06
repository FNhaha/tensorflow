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

#include "tensorflow/compiler/mlir/tensorflow/utils/device_util.h"

#include <memory>
#include <tuple>
#include <utility>

#include "llvm/ADT/SmallVector.h"
#include "mlir/IR/Attributes.h"  // TF:llvm-project
#include "mlir/IR/Builders.h"  // TF:llvm-project
#include "mlir/IR/Location.h"  // TF:llvm-project
#include "mlir/IR/MLIRContext.h"  // TF:llvm-project
#include "mlir/IR/Module.h"  // TF:llvm-project
#include "mlir/Support/LogicalResult.h"  // TF:llvm-project
#include "tensorflow/core/common_runtime/device.h"
#include "tensorflow/core/common_runtime/device_set.h"
#include "tensorflow/core/framework/device_attributes.pb.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/platform/test.h"
#include "tensorflow/core/util/device_name_utils.h"

namespace tensorflow {
namespace {

// A fake device used to populate a DeviceSet.
class FakeDevice : public Device {
 public:
  explicit FakeDevice(const DeviceAttributes& device_attributes)
      : Device(nullptr, device_attributes) {}

  Status Sync() override { return errors::Unimplemented("FakeDevice::Sync()"); }

  static std::unique_ptr<Device> Make(const string& name,
                                      const string& desc = "") {
    DeviceNameUtils::ParsedName parsed_name;
    DeviceNameUtils::ParseFullName(name, &parsed_name);

    DeviceAttributes device_attributes;
    device_attributes.set_name(name);
    device_attributes.set_device_type(parsed_name.type);
    device_attributes.set_physical_device_desc(desc);
    return std::make_unique<FakeDevice>(device_attributes);
  }
};

TEST(DeviceUtilTest, AddDeviceToOp) {
  mlir::MLIRContext context;
  mlir::OwningModuleRef module_ref =
      mlir::ModuleOp::create(mlir::UnknownLoc::get(&context));

  const std::string cpu0 = "/job:worker/replica:0/task:0/device:CPU:0";
  const std::string gpu0 = "/job:worker/replica:1/task:2/device:GPU:0";
  const std::string gpu1 = "/job:worker/replica:1/task:2/device:GPU:1";

  llvm::SmallVector<std::unique_ptr<Device>, 2> devices;
  devices.push_back(FakeDevice::Make(cpu0));
  devices.push_back(FakeDevice::Make(gpu0, "compute capability: 7.0"));
  devices.push_back(FakeDevice::Make(gpu1));

  DeviceSet device_set;
  for (auto& device : devices) device_set.AddDevice(device.get());
  AddDevicesToOp(*module_ref, &device_set);

  auto devices_attr =
      module_ref->getAttrOfType<mlir::DictionaryAttr>("tf.devices");
  ASSERT_NE(devices_attr, nullptr);
  ASSERT_EQ(devices_attr.size(), 3);

  // CPU device added with an empty metadata.
  auto device_meta_0 = devices_attr.get(cpu0).dyn_cast<mlir::DictionaryAttr>();
  ASSERT_NE(device_meta_0, nullptr);
  ASSERT_EQ(device_meta_0.size(), 0);

  // GPU device successfully parsed compute capability from description.
  auto device_meta_1 =
      devices_attr.get(gpu0).dyn_cast<mlir::TF::GpuDeviceMetadata>();
  ASSERT_NE(device_meta_1, nullptr);
  ASSERT_EQ(device_meta_1.cc_major().getInt(), 7);
  ASSERT_EQ(device_meta_1.cc_minor().getInt(), 0);

  // If description is empty GPU devices added with an empty metadata.
  auto device_meta_2 = devices_attr.get(gpu1).dyn_cast<mlir::DictionaryAttr>();
  ASSERT_NE(device_meta_2, nullptr);
  ASSERT_EQ(device_meta_2.size(), 0);
}

TEST(DeviceUtilTest, AddDeviceToOpNullDeviceSet) {
  mlir::MLIRContext context;
  mlir::OwningModuleRef module_ref =
      mlir::ModuleOp::create(mlir::UnknownLoc::get(&context));

  AddDevicesToOp(*module_ref, /*device_set=*/nullptr);
  EXPECT_EQ(module_ref->getAttr("tf.devices"), nullptr);
}

TEST(DeviceUtilTest, GetDevicesFromOpNoDevicesAttribute) {
  mlir::MLIRContext context;
  mlir::OwningModuleRef module_ref =
      mlir::ModuleOp::create(mlir::UnknownLoc::get(&context));

  llvm::SmallVector<DeviceNameUtils::ParsedName, 8> devices;
  EXPECT_TRUE(mlir::succeeded(GetDevicesFromOp(*module_ref, &devices)));
}

TEST(DeviceUtilTest, GetDevicesFromOpBadDevicesAttributeType) {
  mlir::MLIRContext context;
  mlir::OwningModuleRef module_ref =
      mlir::ModuleOp::create(mlir::UnknownLoc::get(&context));
  mlir::Builder builder(*module_ref);
  module_ref->setAttr("tf.devices", builder.getBoolAttr(false));

  llvm::SmallVector<DeviceNameUtils::ParsedName, 8> devices;
  EXPECT_TRUE(mlir::failed(GetDevicesFromOp(*module_ref, &devices)));
}

TEST(DeviceUtilTest, GetDevicesFromOpBadDevicesAttributeArraySubtype) {
  mlir::MLIRContext context;
  mlir::OwningModuleRef module_ref =
      mlir::ModuleOp::create(mlir::UnknownLoc::get(&context));
  mlir::Builder builder(*module_ref);
  module_ref->setAttr("tf.devices", builder.getI32ArrayAttr({8}));

  llvm::SmallVector<DeviceNameUtils::ParsedName, 8> devices;
  EXPECT_TRUE(mlir::failed(GetDevicesFromOp(*module_ref, &devices)));
}

TEST(DeviceUtilTest, GetDevicesFromOpBadDevicesInDevicesAttribute) {
  mlir::MLIRContext context;
  mlir::OwningModuleRef module_ref =
      mlir::ModuleOp::create(mlir::UnknownLoc::get(&context));
  mlir::Builder builder(*module_ref);
  module_ref->setAttr("tf.devices",
                      builder.getDictionaryAttr(builder.getNamedAttr(
                          "bad_device", builder.getDictionaryAttr({}))));

  llvm::SmallVector<DeviceNameUtils::ParsedName, 8> devices;
  EXPECT_TRUE(mlir::failed(GetDevicesFromOp(*module_ref, &devices)));
}

TEST(DeviceUtilTest, GetDevicesFromOpValidDeviceInDevicesAttribute) {
  mlir::MLIRContext context;
  mlir::OwningModuleRef module_ref =
      mlir::ModuleOp::create(mlir::UnknownLoc::get(&context));
  mlir::Builder builder(*module_ref);

  auto device_dict = builder.getDictionaryAttr(
      {builder.getNamedAttr("/job:worker/replica:0/task:0/device:CPU:0",
                            builder.getDictionaryAttr({}))});
  module_ref->setAttr("tf.devices", device_dict);

  llvm::SmallVector<DeviceNameUtils::ParsedName, 8> devices;
  EXPECT_TRUE(mlir::succeeded(GetDevicesFromOp(*module_ref, &devices)));
  ASSERT_EQ(devices.size(), 1);
  EXPECT_EQ(DeviceNameUtils::ParsedNameToString(devices[0]),
            "/job:worker/replica:0/task:0/device:CPU:0");
}

TEST(DeviceUtilTest, GetGpuDeviceMetadata) {
  mlir::MLIRContext context;
  mlir::OwningModuleRef module_ref =
      mlir::ModuleOp::create(mlir::UnknownLoc::get(&context));

  mlir::Builder builder(*module_ref);

  const std::string gpu0 = "/job:worker/replica:0/task:0/device:GPU:0";
  const std::string gpu1 = "/job:worker/replica:0/task:0/device:GPU:1";

  llvm::SmallVector<mlir::NamedAttribute, 2> metadata;
  metadata.push_back(builder.getNamedAttr(
      gpu0, mlir::TF::GpuDeviceMetadata::get(builder.getI32IntegerAttr(1),
                                             builder.getI32IntegerAttr(2),
                                             module_ref->getContext())));

  module_ref->setAttr("tf.devices", builder.getDictionaryAttr(metadata));

  DeviceNameUtils::ParsedName parsed_name;
  DeviceNameUtils::ParseFullName(gpu0, &parsed_name);
  auto meta_0 = GetGpuDeviceMetadata(*module_ref, parsed_name);
  ASSERT_TRUE(meta_0.hasValue());
  ASSERT_EQ(meta_0->cc_major().getInt(), 1);
  ASSERT_EQ(meta_0->cc_minor().getInt(), 2);

  DeviceNameUtils::ParseFullName(gpu1, &parsed_name);
  auto meta_1 = GetGpuDeviceMetadata(*module_ref, parsed_name);
  ASSERT_FALSE(meta_1.hasValue());
}

}  // anonymous namespace
}  // namespace tensorflow
