#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace metal {

struct PoolOptions {
    std::size_t max{};
    std::size_t min{};
    std::chrono::milliseconds idle_timeout{};
    std::chrono::milliseconds reap_interval{};
    std::chrono::milliseconds acquire_timeout{};
};

template <typename TResource>
struct PoolAdapter {
    std::function<std::unique_ptr<TResource>()> create;
    std::function<void(std::unique_ptr<TResource>)> destroy;
    std::function<bool(TResource&)> validate;
};

template <typename TResource>
class Pool {
    struct State;

public:
    class Lease {
    public:
        Lease() = default;
        ~Lease() { release_noexcept(); }

        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;

        Lease(Lease&& other) noexcept
            : state_(std::move(other.state_)), resource_(std::move(other.resource_)) {}

        Lease& operator=(Lease&& other) noexcept {
            if (this == &other) return *this;
            release_noexcept();
            state_ = std::move(other.state_);
            resource_ = std::move(other.resource_);
            return *this;
        }

        [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(resource_); }

        TResource& resource() {
            if (!resource_) throw std::logic_error("MetalORM: pool lease is no longer active");
            return *resource_;
        }

        const TResource& resource() const {
            if (!resource_) throw std::logic_error("MetalORM: pool lease is no longer active");
            return *resource_;
        }

        TResource& operator*() { return resource(); }
        const TResource& operator*() const { return resource(); }
        TResource* operator->() { return &resource(); }
        const TResource* operator->() const { return &resource(); }

        /** Return the resource to the pool. Idempotent. */
        void release() {
            if (!resource_) return;
            auto state = std::move(state_);
            auto resource = std::move(resource_);
            state->release_resource(std::move(resource));
        }

        /** Permanently remove the resource from the pool. Idempotent. */
        void destroy() {
            if (!resource_) return;
            auto state = std::move(state_);
            auto resource = std::move(resource_);
            state->destroy_leased_resource(std::move(resource));
        }

    private:
        friend class Pool;

        Lease(std::shared_ptr<State> state, std::unique_ptr<TResource> resource)
            : state_(std::move(state)), resource_(std::move(resource)) {}

        void release_noexcept() noexcept {
            if (!resource_) return;
            try {
                release();
            } catch (...) {
                resource_.reset();
                state_.reset();
            }
        }

        std::shared_ptr<State> state_;
        std::unique_ptr<TResource> resource_;
    };

    Pool(PoolAdapter<TResource> adapter, PoolOptions options)
        : state_(std::make_shared<State>(std::move(adapter), normalize_options(options))) {
        if (!state_->adapter.create) {
            throw std::invalid_argument("MetalORM: pool adapter.create must be provided");
        }
        warm_minimum();
        start_reaper();
    }

    ~Pool() { destroy_noexcept(); }

    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;
    Pool(Pool&&) = delete;
    Pool& operator=(Pool&&) = delete;

    /** Acquire a resource lease, blocking up to acquire_timeout when configured. */
    Lease acquire() {
        const auto state = state_;
        const auto timeout = state->options.acquire_timeout;
        const auto deadline = timeout.count() > 0
            ? std::chrono::steady_clock::now() + timeout
            : std::chrono::steady_clock::time_point::max();

        for (;;) {
            std::unique_ptr<TResource> resource;
            bool reserved_idle = false;
            bool create_new = false;

            {
                std::unique_lock lock(state->mutex);
                if (state->destroyed) throw std::runtime_error("MetalORM: pool is destroyed");

                if (!state->idle.empty()) {
                    resource = std::move(state->idle.back().resource);
                    state->idle.pop_back();
                    ++state->leased;
                    reserved_idle = true;
                } else if (state->total_live_locked() < state->options.max) {
                    ++state->creating;
                    create_new = true;
                } else {
                    const auto ready = [&] {
                        return state->destroyed || !state->idle.empty()
                            || state->total_live_locked() < state->options.max;
                    };

                    if (timeout.count() > 0) {
                        if (!state->cv.wait_until(lock, deadline, ready)) {
                            throw std::runtime_error("MetalORM: pool acquire timeout");
                        }
                    } else {
                        state->cv.wait(lock, ready);
                    }
                    continue;
                }
            }

            if (reserved_idle) {
                try {
                    if (state->adapter.validate && !state->adapter.validate(*resource)) {
                        {
                            std::lock_guard lock(state->mutex);
                            if (state->leased != 0) --state->leased;
                        }
                        state->destroy_one(std::move(resource));
                        state->cv.notify_one();
                        continue;
                    }
                } catch (...) {
                    {
                        std::lock_guard lock(state->mutex);
                        if (state->leased != 0) --state->leased;
                    }
                    state->destroy_one_noexcept(std::move(resource));
                    state->cv.notify_one();
                    throw;
                }

                bool destroyed = false;
                {
                    std::lock_guard lock(state->mutex);
                    destroyed = state->destroyed;
                    if (destroyed && state->leased != 0) --state->leased;
                }
                if (destroyed) {
                    state->destroy_one(std::move(resource));
                    state->cv.notify_all();
                    throw std::runtime_error("MetalORM: pool is destroyed");
                }
                return Lease{state, std::move(resource)};
            }

            if (create_new) {
                try {
                    resource = state->adapter.create();
                    if (!resource) {
                        throw std::runtime_error("MetalORM: pool adapter.create returned null");
                    }
                } catch (...) {
                    {
                        std::lock_guard lock(state->mutex);
                        --state->creating;
                    }
                    state->cv.notify_all();
                    throw;
                }

                bool destroyed = false;
                {
                    std::lock_guard lock(state->mutex);
                    --state->creating;
                    destroyed = state->destroyed;
                    if (!destroyed) ++state->leased;
                }
                state->cv.notify_all();

                if (destroyed) {
                    state->destroy_one(std::move(resource));
                    throw std::runtime_error("MetalORM: pool is destroyed");
                }
                return Lease{state, std::move(resource)};
            }
        }
    }

    /** Destroy the pool and every idle resource. Existing leases are destroyed when returned. */
    void destroy() {
        const auto state = state_;
        std::vector<std::unique_ptr<TResource>> idle;
        {
            std::lock_guard lock(state->mutex);
            if (state->destroyed) return;
            state->destroyed = true;
            idle.reserve(state->idle.size());
            while (!state->idle.empty()) {
                idle.push_back(std::move(state->idle.front().resource));
                state->idle.pop_front();
            }
        }

        state->reaper.request_stop();
        state->cv.notify_all();
        state->reaper_cv.notify_all();

        std::exception_ptr first_error;
        for (auto& resource : idle) {
            try {
                state->destroy_one(std::move(resource));
            } catch (...) {
                if (!first_error) first_error = std::current_exception();
            }
        }
        if (first_error) std::rethrow_exception(first_error);
    }

private:
    struct IdleEntry {
        std::unique_ptr<TResource> resource;
        std::chrono::steady_clock::time_point last_used_at;
    };

    struct State {
        State(PoolAdapter<TResource> adapter_value, PoolOptions options_value)
            : adapter(std::move(adapter_value)), options(options_value) {}

        [[nodiscard]] std::size_t total_live_locked() const noexcept {
            return idle.size() + leased + creating;
        }

        void destroy_one(std::unique_ptr<TResource> resource) {
            if (!resource) return;
            if (adapter.destroy) adapter.destroy(std::move(resource));
        }

        void destroy_one_noexcept(std::unique_ptr<TResource> resource) noexcept {
            if (!resource) return;
            try {
                destroy_one(std::move(resource));
            } catch (...) {}
        }

        void release_resource(std::unique_ptr<TResource> resource) {
            bool should_destroy = false;
            {
                std::lock_guard lock(mutex);
                if (leased != 0) --leased;
                should_destroy = destroyed;
                if (!should_destroy) {
                    idle.push_back({std::move(resource), std::chrono::steady_clock::now()});
                }
            }
            if (should_destroy) destroy_one(std::move(resource));
            cv.notify_one();
        }

        void destroy_leased_resource(std::unique_ptr<TResource> resource) {
            {
                std::lock_guard lock(mutex);
                if (leased != 0) --leased;
            }
            destroy_one(std::move(resource));
            cv.notify_one();
        }

        PoolAdapter<TResource> adapter;
        PoolOptions options;
        std::mutex mutex;
        std::condition_variable cv;
        std::condition_variable reaper_cv;
        std::deque<IdleEntry> idle;
        std::size_t creating{};
        std::size_t leased{};
        bool destroyed{};
        std::jthread reaper;
    };

    static PoolOptions normalize_options(PoolOptions options) {
        if (options.max == 0) {
            throw std::invalid_argument("MetalORM: pool options.max must be greater than zero");
        }
        options.min = std::min(options.min, options.max);
        return options;
    }

    void warm_minimum() {
        const auto state = state_;
        for (;;) {
            {
                std::lock_guard lock(state->mutex);
                if (state->destroyed || state->idle.size() >= state->options.min
                    || state->total_live_locked() >= state->options.max) {
                    return;
                }
                ++state->creating;
            }

            try {
                auto resource = state->adapter.create();
                if (!resource) throw std::runtime_error("MetalORM: pool adapter.create returned null");
                {
                    std::lock_guard lock(state->mutex);
                    --state->creating;
                    state->idle.push_back({std::move(resource), std::chrono::steady_clock::now()});
                }
            } catch (...) {
                std::lock_guard lock(state->mutex);
                --state->creating;
                return;
            }
        }
    }

    void start_reaper() {
        const auto state = state_;
        if (state->options.idle_timeout.count() <= 0) return;

        auto interval = state->options.reap_interval;
        if (interval.count() <= 0) {
            interval = std::max(
                std::chrono::milliseconds{1000},
                state->options.idle_timeout / 2);
        }

        std::weak_ptr<State> weak = state;
        state->reaper = std::jthread([weak, interval](std::stop_token stop) {
            while (!stop.stop_requested()) {
                auto current = weak.lock();
                if (!current) return;

                std::vector<std::unique_ptr<TResource>> expired;
                {
                    std::unique_lock lock(current->mutex);
                    current->reaper_cv.wait_for(lock, interval, [&] {
                        return current->destroyed || stop.stop_requested();
                    });
                    if (current->destroyed || stop.stop_requested()) return;

                    const auto now = std::chrono::steady_clock::now();
                    const auto removable = current->idle.size() > current->options.min
                        ? current->idle.size() - current->options.min
                        : 0;
                    std::size_t removed = 0;
                    std::deque<IdleEntry> keep;
                    while (!current->idle.empty()) {
                        auto entry = std::move(current->idle.front());
                        current->idle.pop_front();
                        const bool timed_out = now - entry.last_used_at >= current->options.idle_timeout;
                        if (timed_out && removed < removable) {
                            expired.push_back(std::move(entry.resource));
                            ++removed;
                        } else {
                            keep.push_back(std::move(entry));
                        }
                    }
                    current->idle = std::move(keep);
                }

                for (auto& resource : expired) {
                    current->destroy_one_noexcept(std::move(resource));
                }
                if (!expired.empty()) current->cv.notify_all();
            }
        });
    }

    void destroy_noexcept() noexcept {
        if (!state_) return;
        try {
            destroy();
        } catch (...) {}
    }

    std::shared_ptr<State> state_;
};

} // namespace metal
