#pragma once

#include <cstddef>
#include <type_traits>
#include <utility>

namespace delta {

/**
 * 用于函数级静态变量的线程安全包装器，提供了一个 get() 方法来访问内部对象的引用。
 */

template <typename InstanceType>
class NoDestructor {
   public:
    template <typename... ConstructorArgTypes>
    explicit NoDestructor(ConstructorArgTypes&&... constructor_args) {
        static_assert(sizeof(instance_storage_) >= sizeof(InstanceType),
                      "instance_storage_ is not large enough to hold the instance");
        static_assert(std::is_standard_layout_v<NoDestructor<InstanceType>>);
        static_assert(offsetof(NoDestructor, instance_storage_) % alignof(InstanceType) == 0,
                      "instance_storage_ does not meet the instance's alignment requirement");
        static_assert(alignof(NoDestructor<InstanceType>) % alignof(InstanceType) == 0,
                      "instance_storage_ does not meet the instance's alignment requirement");
        new (instance_storage_) InstanceType(std::forward<ConstructorArgTypes>(constructor_args)...);
    }

    ~NoDestructor() = default;

    NoDestructor(const NoDestructor&) = delete;
    NoDestructor& operator=(const NoDestructor&) = delete;

    InstanceType* get() { return reinterpret_cast<InstanceType*>(&instance_storage_); }

   private:
    alignas(InstanceType) char instance_storage_[sizeof(InstanceType)];
};

}  // namespace delta