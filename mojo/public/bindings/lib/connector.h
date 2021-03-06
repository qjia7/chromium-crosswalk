// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MOJO_PUBLIC_BINDINGS_LIB_CONNECTOR_H_
#define MOJO_PUBLIC_BINDINGS_LIB_CONNECTOR_H_

#include "mojo/public/bindings/lib/message.h"
#include "mojo/public/bindings/lib/message_queue.h"
#include "mojo/public/environment/default_async_waiter.h"
#include "mojo/public/system/core_cpp.h"

namespace mojo {
namespace internal {

// The Connector class is responsible for performing read/write operations on a
// MessagePipe. It writes messages it receives through the MessageReceiver
// interface that it subclasses, and it forwards messages it reads through the
// MessageReceiver interface assigned as its incoming receiver.
//
// NOTE: MessagePipe I/O is non-blocking.
//
class Connector : public MessageReceiver {
 public:
  // The Connector takes ownership of |message_pipe|.
  explicit Connector(ScopedMessagePipeHandle message_pipe,
                     MojoAsyncWaiter* waiter = GetDefaultAsyncWaiter());
  virtual ~Connector();

  // Sets the receiver to handle messages read from the message pipe.  The
  // Connector will only read messages from the pipe if an incoming receiver
  // has been set.
  void SetIncomingReceiver(MessageReceiver* receiver);

  // Returns true if an error was encountered while reading from or writing to
  // the message pipe.
  bool encountered_error() const { return error_; }

  // MessageReceiver implementation:
  virtual bool Accept(Message* message) MOJO_OVERRIDE;

 private:
  static void OnHandleReady(void* closure, MojoResult result);

  void WaitToReadMore();
  void ReadMore();
  void WriteOne(Message* message);

  MojoAsyncWaiter* waiter_;

  ScopedMessagePipeHandle message_pipe_;
  MessageReceiver* incoming_receiver_;

  MojoAsyncWaitID async_wait_id_;
  bool error_;

  MOJO_DISALLOW_COPY_AND_ASSIGN(Connector);
};

}  // namespace internal
}  // namespace mojo

#endif  // MOJO_PUBLIC_BINDINGS_LIB_CONNECTOR_H_
