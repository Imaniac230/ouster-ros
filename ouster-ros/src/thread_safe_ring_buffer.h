/**
 * Copyright (c) 2018-2023, Ouster, Inc.
 * All rights reserved.
 *
 * @file thread_safe_ring_buffer.h
 * @brief File contains thread safe implementation of a ring buffer
 */

#pragma once

#include <condition_variable>
#include <mutex>
#include <atomic>
#include <vector>

/**
 * @class ThreadSafeRingBuffer thread safe ring buffer.
 */
class ThreadSafeRingBuffer {
   public:
    ThreadSafeRingBuffer(size_t item_size_, size_t items_count_)
        : buffer(item_size_ * items_count_),
          item_size(item_size_),
          max_items_count(items_count_),
          active_items_count(0),
          write_idx(0),
          read_idx(0),
          new_data_lock(mutex, std::defer_lock),
          free_space_lock(mutex, std::defer_lock) {}

    /**
     * Gets the maximum number of items that this ring buffer can hold.
     */
    [[nodiscard]] size_t capacity() const { return max_items_count; }

    /**
     * Gets the number of item that currently occupy the ring buffer. This
     * number would vary between 0 and the capacity().
     *
     * @remarks
     *  if returned value was 0 or the value was equal to the buffer capacity(),
     *  this does not guarantee that a subsequent call to read() or write()
     *  wouldn't cause the calling thread to be blocked.
     */
    [[nodiscard]] size_t size() const {
        return active_items_count.load();
    }

    /**
     * Checks if the ring buffer is empty.
     *
     * @remarks
     *  if empty() returns true this does not guarantee that calling the write()
     *  operation directly right after wouldn't block the calling thread.
     */
    [[nodiscard]] bool empty() const {
        return active_items_count.load() == 0;
    }

    /**
     * Checks if the ring buffer is full.
     *
     * @remarks
     *  if full() returns true this does not guarantee that calling the read()
     *  operation directly right after wouldn't block the calling thread.
     */
    [[nodiscard]] bool full() const {
        return active_items_count.load() == max_items_count;
    }

    /**
     * Writes to the buffer safely, the method will keep blocking until the
     * there is a space available within the buffer.
     */
    template <class BufferWriteFn>
    void write(BufferWriteFn&& buffer_write) {
        free_space_lock.lock();
        free_space_condition.wait(free_space_lock,
                           [this] { return active_items_count.load() < capacity(); });
          buffer_write(&buffer[write_idx.load() * item_size]);
          write_idx += 1;
          size_t c = capacity();
          write_idx.compare_exchange_strong(c, 0);
          ++active_items_count;
          new_data_condition.notify_all();
          free_space_lock.unlock();
    }

    /**
     * Writes to the buffer safely, if there is no space left, then this method
     * will overwite the last item.
     */
    template <class BufferWriteFn>
    void write_overwrite(BufferWriteFn&& buffer_write) {
        buffer_write(&buffer[write_idx.load() * item_size]);
        write_idx += 1;
        size_t c = capacity();
        write_idx.compare_exchange_strong(c, 0);
        if (active_items_count.load() < capacity()) {
            ++active_items_count;
        } else {
            read_idx += 1;
            c = capacity();
            read_idx.compare_exchange_strong(c, 0);
        }
        new_data_condition.notify_all();
    }

    /**
     * Writes to the buffer safely, this method will return immediately and if
     * there is no space left, the data will not be written (will be dropped).
     */
    template <class BufferWriteFn>
    void write_nonblock(BufferWriteFn&& buffer_write) {
      if (active_items_count.load() < capacity()) {
        buffer_write(&buffer[write_idx.load() * item_size]);
        write_idx += 1;
        size_t c = capacity();
        write_idx.compare_exchange_strong(c, 0);
        ++active_items_count;
        new_data_condition.notify_all();
      }
    }

    /**
     * Gives access to read the buffer through a callback, the method will block
     * until there is something to read available.
     */
    template <typename BufferReadFn>
    void read(BufferReadFn&& buffer_read) {
        new_data_lock.lock();
        new_data_condition.wait(new_data_lock, [this] { return active_items_count.load() > 0; });
        buffer_read(&buffer[read_idx * item_size]);
        read_idx += 1;
        size_t c = capacity();
        read_idx.compare_exchange_strong(c, 0);
        --active_items_count;
        free_space_condition.notify_one();
        new_data_lock.unlock();
    }

    /**
     * Gives access to read the buffer through a callback, if buffer is
     * inaccessible the method will timeout and the callback is not performed.
     */
    template <typename BufferReadFn>
    void read_timeout(BufferReadFn&& buffer_read,
                      std::chrono::seconds timeout) {
        new_data_lock.lock();
        if (new_data_condition.wait_for(
                new_data_lock, timeout, [this] { return active_items_count.load() > 0; })) {
            buffer_read(&buffer[read_idx * item_size]);
            read_idx += 1;
            size_t c = capacity();
            read_idx.compare_exchange_strong(c, 0);
            --active_items_count;
            free_space_condition.notify_one();
        }
        new_data_lock.unlock();
    }

    /**
     * Gives access to read the buffer through a callback, the method will return
     * immediately and the callback is performed only when there is data available.
     */
    template <typename BufferReadFn>
    void read_nonblock(BufferReadFn&& buffer_read) {
      if (active_items_count.load() > 0) {
        buffer_read(&buffer[read_idx.load() * item_size]);
        read_idx += 1;
        size_t c = capacity();
        read_idx.compare_exchange_strong(c, 0);
        --active_items_count;
        free_space_condition.notify_one();
      }
    }

   private:
    std::vector<uint8_t> buffer;

    const size_t item_size;
    const size_t max_items_count;

    std::atomic_size_t active_items_count;
    std::atomic_size_t write_idx;
    std::atomic_size_t read_idx;

    std::condition_variable new_data_condition;
    std::unique_lock<std::mutex> new_data_lock;
    std::condition_variable free_space_condition;
    std::unique_lock<std::mutex> free_space_lock;
    std::mutex mutex;
};