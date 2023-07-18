/**
 * @file doer.h
 * @brief Declaration file for the Doer class.  The is the base class for all
 * agora doers
 */
#ifndef DOER_H_
#define DOER_H_

#include <cstddef>
#include <typeinfo>

#include "concurrent_queue_wrapper.h"
#include "concurrentqueue.h"
#include "config.h"
#include "message.h"
#include "utils.h"

class Doer {
 public:
  virtual bool TryLaunch(
      moodycamel::ConcurrentQueue<EventData>& task_queue,
      moodycamel::ConcurrentQueue<EventData>& complete_task_queue,
      moodycamel::ProducerToken* worker_ptok) {
    
    // printf("===== Doer - TryLaunch =====\n");
    // printf("[debug] Doer: Try Launch.\n");
    EventData req_event;

    // printf("[debug] Doer: Try to dequeue.\n");
    // printf("[debug] Doer: task_queue.size_approx() = %ld\n", task_queue.size_approx());

    ///Each event is handled by 1 Doer(Thread) and each tag is processed sequentually
    if (task_queue.try_dequeue(req_event)) {

      // printf("[debug] Doer: Dequeue'in.\n");
      // printf("[debug] Doer: Dequeued event type: %d\n", static_cast<int>(req_event.event_type_));
      // We will enqueue one response event containing results for all
      // request tags in the request event
      EventData resp_event;
      resp_event.num_tags_ = req_event.num_tags_;
      resp_event.event_type_ = req_event.event_type_;

      for (size_t i = 0; i < req_event.num_tags_; i++) {
        // printf("[debug] Before launch...\n");
        EventData doer_comp = Launch(req_event.tags_.at(i));
        // printf("[debug] After launch...\n");
        RtAssert(doer_comp.num_tags_ == 1, "Invalid num_tags in resp");
        resp_event.tags_.at(i) = doer_comp.tags_.at(0);
        RtAssert(resp_event.event_type_ == doer_comp.event_type_,
                 "Invalid event type in resp");
      }

      // debug
      // printf("===== Doer - Launched =====\n");
      // printf("[debug] TryEnqueueFallback...\n");
      TryEnqueueFallback(&complete_task_queue, worker_ptok, resp_event);
      return true;
    }
    // printf("[debug] Doer: dequeue fails, existing...\n");
    return false;
  }

  /// The main event handling function that performs Doer-specific work.
  /// Doers that handle only one event type use this signature.
  virtual EventData Launch(size_t tag) {
    unused(tag);
    RtAssert(false, "Doer: Launch(tag) not implemented");
    return EventData();
  }

  /// The main event handling function that performs Doer-specific work.
  /// Doers that handle multiple event types use this signature.
  virtual EventData Launch(size_t tag, EventType event_type) {
    unused(tag);
    unused(event_type);
    RtAssert(false, "Doer: Launch(tag, event_type) not implemented");
    return EventData();
  }

 protected:
  Doer(Config* in_config, int in_tid) : cfg_(in_config), tid_(in_tid) {}
  virtual ~Doer() = default;

  Config* cfg_;
  int tid_;  // Thread ID of this Doer
};
#endif  // DOER_H_
