// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -O1 -emit-llvm %s -o - -disable-llvm-passes | FileCheck %s

#include "Inputs/coroutine.h"

struct Task {
  struct promise_type {
    Task get_return_object() noexcept {
      return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
    }

    void return_void() noexcept {}

    struct final_awaiter {
      bool await_ready() noexcept { return false; }
      std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept {
        h.destroy();
        return {};
      }
      void await_resume() noexcept {}
    };

    void unhandled_exception() noexcept {}

    final_awaiter final_suspend() noexcept { return {}; }

    std::suspend_always initial_suspend() noexcept { return {}; }

    template <typename Awaitable>
    auto await_transform(Awaitable &&awaitable) {
      return awaitable.co_viaIfAsync();
    }
  };

  using handle_t = std::coroutine_handle<promise_type>;

  class Awaiter {
  public:
    explicit Awaiter(handle_t coro) noexcept;
    Awaiter(Awaiter &&other) noexcept;
    Awaiter(const Awaiter &) = delete;
    ~Awaiter();

    bool await_ready() noexcept { return false; }
    handle_t await_suspend(std::coroutine_handle<> continuation) noexcept;
    void await_resume();

  private:
    handle_t coro_;
  };

  Task(handle_t coro) noexcept : coro_(coro) {}

  handle_t coro_;

  Task(const Task &t) = delete;
  Task(Task &&t) noexcept;
  ~Task();
  Task &operator=(Task t) noexcept;

  Awaiter co_viaIfAsync();
};

static Task foo() {
  co_return;
}

Task bar() {
  auto mode = 2;
  switch (mode) {
  case 1:
    co_await foo();
    break;
  case 2:
    co_await foo();
    break;
  default:
    break;
  }
}

// CHECK-LABEL: define{{.*}} void @_Z3barv
// CHECK:         %[[MODE:.+]] = load i32, ptr %mode
// CHECK-NEXT:    switch i32 %[[MODE]], label %{{.+}} [
// CHECK-NEXT:      i32 1, label %[[CASE1:.+]]
// CHECK-NEXT:      i32 2, label %[[CASE2:.+]]
// CHECK-NEXT:    ]

// CHECK:       [[CASE1]]:
// CHECK:         br i1 %{{.+}}, label %[[CASE1_AWAIT_READY:.+]], label %[[CASE1_AWAIT_SUSPEND:.+]]
// CHECK:       [[CASE1_AWAIT_SUSPEND]]:
// CHECK-NEXT:    %{{.+}} = call token @llvm.coro.save(ptr null)
// CHECK-NEXT:    %[[HANDLE1_PTR:.+]] = call ptr @llvm.coro.await.suspend.handle
// CHECK-NEXT:    call void @llvm.coro.resume(ptr %[[HANDLE1_PTR]])
// CHECK-NEXT:    %{{.+}} = call i8 @llvm.coro.suspend
// CHECK-NEXT:    switch i8 %{{.+}}, label %coro.ret [
// CHECK-NEXT:      i8 0, label %[[CASE1_AWAIT_READY]]
// CHECK-NEXT:      i8 1, label %[[CASE1_AWAIT_CLEANUP:.+]]
// CHECK-NEXT:    ]
// CHECK:       [[CASE1_AWAIT_CLEANUP]]:
// make sure that the awaiter eventually gets cleaned up.
// CHECK:         call void @{{.+Awaiter.+}}

// CHECK:       [[CASE2]]:
// CHECK:         br i1 %{{.+}}, label %[[CASE2_AWAIT_READY:.+]], label %[[CASE2_AWAIT_SUSPEND:.+]]
// CHECK:       [[CASE2_AWAIT_SUSPEND]]:
// CHECK-NEXT:    %{{.+}} = call token @llvm.coro.save(ptr null)
// CHECK-NEXT:    %[[HANDLE2_PTR:.+]] = call ptr @llvm.coro.await.suspend.handle
// CHECK-NEXT:    call void @llvm.coro.resume(ptr %[[HANDLE2_PTR]])
// CHECK-NEXT:    %{{.+}} = call i8 @llvm.coro.suspend
// CHECK-NEXT:    switch i8 %{{.+}}, label %coro.ret [
// CHECK-NEXT:      i8 0, label %[[CASE2_AWAIT_READY]]
// CHECK-NEXT:      i8 1, label %[[CASE2_AWAIT_CLEANUP:.+]]
// CHECK-NEXT:    ]
// CHECK:       [[CASE2_AWAIT_CLEANUP]]:
// make sure that the awaiter eventually gets cleaned up.
// CHECK:         call void @{{.+Awaiter.+}}
