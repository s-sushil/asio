//
// detail/impl/win_iocp_handle_service.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2010 Christopher M. Kohlhoff (chris at kohlhoff dot com)
// Copyright (c) 2008 Rep Invariant Systems, Inc. (info@repinvariant.com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_DETAIL_IMPL_WIN_IOCP_HANDLE_SERVICE_IPP
#define ASIO_DETAIL_IMPL_WIN_IOCP_HANDLE_SERVICE_IPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include "asio/detail/config.hpp"

#if defined(ASIO_HAS_IOCP)

#include "asio/detail/win_iocp_handle_service.hpp"

#include "asio/detail/push_options.hpp"

namespace asio {
namespace detail {

win_iocp_handle_service::win_iocp_handle_service(
    asio::io_service& io_service)
  : iocp_service_(asio::use_service<win_iocp_io_service>(io_service)),
    mutex_(),
    impl_list_(0)
{
}

void win_iocp_handle_service::shutdown_service()
{
  // Close all implementations, causing all operations to complete.
  asio::detail::mutex::scoped_lock lock(mutex_);
  implementation_type* impl = impl_list_;
  while (impl)
  {
    close_for_destruction(*impl);
    impl = impl->next_;
  }
}

void win_iocp_handle_service::construct(
    win_iocp_handle_service::implementation_type& impl)
{
  impl.handle_ = INVALID_HANDLE_VALUE;
  impl.safe_cancellation_thread_id_ = 0;

  // Insert implementation into linked list of all implementations.
  asio::detail::mutex::scoped_lock lock(mutex_);
  impl.next_ = impl_list_;
  impl.prev_ = 0;
  if (impl_list_)
    impl_list_->prev_ = &impl;
  impl_list_ = &impl;
}

void win_iocp_handle_service::destroy(
    win_iocp_handle_service::implementation_type& impl)
{
  close_for_destruction(impl);
  
  // Remove implementation from linked list of all implementations.
  asio::detail::mutex::scoped_lock lock(mutex_);
  if (impl_list_ == &impl)
    impl_list_ = impl.next_;
  if (impl.prev_)
    impl.prev_->next_ = impl.next_;
  if (impl.next_)
    impl.next_->prev_= impl.prev_;
  impl.next_ = 0;
  impl.prev_ = 0;
}

asio::error_code win_iocp_handle_service::assign(
    win_iocp_handle_service::implementation_type& impl,
    const native_type& native_handle, asio::error_code& ec)
{
  if (is_open(impl))
  {
    ec = asio::error::already_open;
    return ec;
  }

  if (iocp_service_.register_handle(native_handle, ec))
    return ec;

  impl.handle_ = native_handle;
  ec = asio::error_code();
  return ec;
}

asio::error_code win_iocp_handle_service::close(
    win_iocp_handle_service::implementation_type& impl,
    asio::error_code& ec)
{
  if (is_open(impl))
  {
    if (!::CloseHandle(impl.handle_))
    {
      DWORD last_error = ::GetLastError();
      ec = asio::error_code(last_error,
          asio::error::get_system_category());
      return ec;
    }

    impl.handle_ = INVALID_HANDLE_VALUE;
    impl.safe_cancellation_thread_id_ = 0;
  }

  ec = asio::error_code();
  return ec;
}

asio::error_code win_iocp_handle_service::cancel(
    win_iocp_handle_service::implementation_type& impl,
    asio::error_code& ec)
{
  if (!is_open(impl))
  {
    ec = asio::error::bad_descriptor;
  }
  else if (FARPROC cancel_io_ex_ptr = ::GetProcAddress(
        ::GetModuleHandleA("KERNEL32"), "CancelIoEx"))
  {
    // The version of Windows supports cancellation from any thread.
    typedef BOOL (WINAPI* cancel_io_ex_t)(HANDLE, LPOVERLAPPED);
    cancel_io_ex_t cancel_io_ex = (cancel_io_ex_t)cancel_io_ex_ptr;
    if (!cancel_io_ex(impl.handle_, 0))
    {
      DWORD last_error = ::GetLastError();
      if (last_error == ERROR_NOT_FOUND)
      {
        // ERROR_NOT_FOUND means that there were no operations to be
        // cancelled. We swallow this error to match the behaviour on other
        // platforms.
        ec = asio::error_code();
      }
      else
      {
        ec = asio::error_code(last_error,
            asio::error::get_system_category());
      }
    }
    else
    {
      ec = asio::error_code();
    }
  }
  else if (impl.safe_cancellation_thread_id_ == 0)
  {
    // No operations have been started, so there's nothing to cancel.
    ec = asio::error_code();
  }
  else if (impl.safe_cancellation_thread_id_ == ::GetCurrentThreadId())
  {
    // Asynchronous operations have been started from the current thread only,
    // so it is safe to try to cancel them using CancelIo.
    if (!::CancelIo(impl.handle_))
    {
      DWORD last_error = ::GetLastError();
      ec = asio::error_code(last_error,
          asio::error::get_system_category());
    }
    else
    {
      ec = asio::error_code();
    }
  }
  else
  {
    // Asynchronous operations have been started from more than one thread,
    // so cancellation is not safe.
    ec = asio::error::operation_not_supported;
  }

  return ec;
}

void win_iocp_handle_service::start_write_op(
    win_iocp_handle_service::implementation_type& impl, boost::uint64_t offset,
    const asio::const_buffer& buffer, operation* op)
{
  update_cancellation_thread_id(impl);
  iocp_service_.work_started();

  if (!is_open(impl))
  {
    iocp_service_.on_completion(op, asio::error::bad_descriptor);
  }
  else if (asio::buffer_size(buffer) == 0)
  {
    // A request to write 0 bytes on a handle is a no-op.
    iocp_service_.on_completion(op);
  }
  else
  {
    DWORD bytes_transferred = 0;
    op->Offset = offset & 0xFFFFFFFF;
    op->OffsetHigh = (offset >> 32) & 0xFFFFFFFF;
    BOOL ok = ::WriteFile(impl.handle_,
        asio::buffer_cast<LPCVOID>(buffer),
        static_cast<DWORD>(asio::buffer_size(buffer)),
        &bytes_transferred, op);
    DWORD last_error = ::GetLastError();
    if (!ok && last_error != ERROR_IO_PENDING
        && last_error != ERROR_MORE_DATA)
    {
      iocp_service_.on_completion(op, last_error, bytes_transferred);
    }
    else
    {
      iocp_service_.on_pending(op);
    }
  }
}

void win_iocp_handle_service::start_read_op(
    win_iocp_handle_service::implementation_type& impl, boost::uint64_t offset,
    const asio::mutable_buffer& buffer, operation* op)
{
  update_cancellation_thread_id(impl);
  iocp_service_.work_started();

  if (!is_open(impl))
  {
    iocp_service_.on_completion(op, asio::error::bad_descriptor);
  }
  else if (asio::buffer_size(buffer) == 0)
  {
    // A request to read 0 bytes on a handle is a no-op.
    iocp_service_.on_completion(op);
  }
  else
  {
    DWORD bytes_transferred = 0;
    op->Offset = offset & 0xFFFFFFFF;
    op->OffsetHigh = (offset >> 32) & 0xFFFFFFFF;
    BOOL ok = ::ReadFile(impl.handle_,
        asio::buffer_cast<LPVOID>(buffer),
        static_cast<DWORD>(asio::buffer_size(buffer)),
        &bytes_transferred, op);
    DWORD last_error = ::GetLastError();
    if (!ok && last_error != ERROR_IO_PENDING
        && last_error != ERROR_MORE_DATA)
    {
      iocp_service_.on_completion(op, last_error, bytes_transferred);
    }
    else
    {
      iocp_service_.on_pending(op);
    }
  }
}

void win_iocp_handle_service::update_cancellation_thread_id(
    win_iocp_handle_service::implementation_type& impl)
{
  if (impl.safe_cancellation_thread_id_ == 0)
    impl.safe_cancellation_thread_id_ = ::GetCurrentThreadId();
  else if (impl.safe_cancellation_thread_id_ != ::GetCurrentThreadId())
    impl.safe_cancellation_thread_id_ = ~DWORD(0);
}

void win_iocp_handle_service::close_for_destruction(implementation_type& impl)
{
  if (is_open(impl))
  {
    ::CloseHandle(impl.handle_);
    impl.handle_ = INVALID_HANDLE_VALUE;
    impl.safe_cancellation_thread_id_ = 0;
  }
}

} // namespace detail
} // namespace asio

#include "asio/detail/pop_options.hpp"

#endif // defined(ASIO_HAS_IOCP)

#endif // ASIO_DETAIL_IMPL_WIN_IOCP_HANDLE_SERVICE_IPP
