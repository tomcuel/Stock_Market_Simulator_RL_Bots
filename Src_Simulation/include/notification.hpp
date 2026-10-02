//=======================================================================
// Asynchronous event bus: producers publish events without waiting for subscribers
// A background worker processes the queue and notifies subscribers, so slow callbacks (e.g. logging, disk or network I/O) do not block the matching engine
//
// Subscription lifetime: subscribers must call unsubscribe() in their destructor if they can be destroyed before the bus (otherwise, the worker could call a callback that uses an already-destroyed object)
// unsubscribe() waits for any ongoing dispatch to finish before removing the subscriber, this prevents callbacks from accessing an object after it has been destroyed
// flush() waits until all queued events have been delivered to all subscribers pending() alone is not sufficient, because an event may have been removed from the queue while its callbacks are still running
// The worker_ thread is declared last so all other members are initialized before the worker starts
//=======================================================================
#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <variant>

#include "types.hpp"

namespace sim {

struct TradeEvent { Trade trade; };
struct OrderRejectedEvent { OrderId order_id; ClientId client; std::string reason; };
struct OrderAcceptedEvent { OrderId order_id; ClientId client; Symbol symbol; };
struct OrderQueuedEvent { OrderId order_id; ClientId client; Symbol symbol; }; // waiting for release band / start date
struct OrderExpiredEvent { OrderId order_id; ClientId client; Symbol symbol; }; // dropped from the waiting registry unfilled

using Event = std::variant<TradeEvent, OrderRejectedEvent, OrderAcceptedEvent, OrderQueuedEvent, OrderExpiredEvent>;

using SubscriptionId = std::uint64_t;

class NotificationBus {
public:
    using Subscriber = std::function<void(const Event&)>;

    NotificationBus() : worker_([this] { run(); }) {}

    ~NotificationBus() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    NotificationBus(const NotificationBus&) = delete;
    NotificationBus& operator=(const NotificationBus&) = delete;

    // Returns a SubscriptionId: keep it if this subscriber might be destroyed before the bus (see the class comment above) and pass it to unsubscribe() in the subscriber's own destructor
    SubscriptionId subscribe(Subscriber subscriber) {
        std::lock_guard<std::mutex> lock(subscribers_mutex_);
        SubscriptionId id = next_subscription_id_++;
        subscribers_.emplace(id, std::move(subscriber));
        return id;
    }

    // Removes a subscriber: blocks until any dispatch round already in progress finishes (see the class comment above for why that's exactly what makes this safe to call from a destructor)
    void unsubscribe(SubscriptionId id) {
        std::lock_guard<std::mutex> lock(subscribers_mutex_);
        subscribers_.erase(id);
    }

    void publish(Event event) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push(std::move(event));
        }
        cv_.notify_one();
    }

    std::size_t pending() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    // Blocks until every event published so far has been fully delivered to every subscriber: the queue is empty and no dispatch round is still in progress
    // pending() == 0 alone is not enough: the worker pops an event (queue now empty) before it finishes calling subscribers with it
    // Used by MarketRecorder::write_report() so the end-of-run report is guaranteed to include every trade that was already executed, not just the ones the worker got around to
    void flush() {
        std::unique_lock<std::mutex> lock(mutex_);
        idle_cv_.wait(lock, [this] {return queue_.empty() && in_flight_ == 0;});
    }

private:
    void run() {
        while (true) {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] {return stopping_ || !queue_.empty();});
            if (queue_.empty() && stopping_) {
                return;
            }

            Event event = std::move(queue_.front());
            queue_.pop();
            ++in_flight_; // counted under mutex_, so flush() can never observe "queue empty" mid-dispatch
            lock.unlock();

            {
                std::lock_guard<std::mutex> sub_lock(subscribers_mutex_);
                for (auto& [id, subscriber] : subscribers_) {
                    subscriber(event);
                }
            }

            {
                std::lock_guard<std::mutex> done_lock(mutex_);
                --in_flight_;
            }
            idle_cv_.notify_all();
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<Event> queue_;
    bool stopping_{false};
    std::size_t in_flight_{0};           // events popped but not yet fully dispatched (guarded by mutex_)
    std::condition_variable idle_cv_;    // signalled after each dispatch round, see flush()

    std::mutex subscribers_mutex_;
    std::unordered_map<SubscriptionId, Subscriber> subscribers_;
    SubscriptionId next_subscription_id_{1};

    std::thread worker_; // must be declared last so it starts after everything else is initialized
};

} // namespace sim
