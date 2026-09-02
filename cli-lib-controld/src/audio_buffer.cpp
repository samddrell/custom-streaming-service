#include "audio_buffer.h"

AudioBuffer::AudioBuffer(size_t capacity_bytes) : capacity_bytes_(capacity_bytes) {}

bool AudioBuffer::push(const char* data, size_t len) {
  std::unique_lock<std::mutex> lock(mutex_);
  not_full_.wait(lock, [&] { return size_bytes_ < capacity_bytes_ || consumer_stopped_; });
  if (consumer_stopped_) return false;
  // Allowed to overshoot capacity_bytes_ by at most one chunk (~64KB, per playback-controld's
  // kChunkSize) -- capacity is an approximate "~6 minutes of audio" target, not a hard
  // real-time guarantee, so this is an acceptable simplification over reserving space in
  // advance for exact-never-overshoot precision.
  chunks_.emplace_back(data, len);
  size_bytes_ += len;
  lock.unlock();
  not_empty_.notify_one();
  return true;
}

std::optional<std::string> AudioBuffer::pop() {
  std::unique_lock<std::mutex> lock(mutex_);
  not_empty_.wait(lock, [&] { return !chunks_.empty() || producer_finished_; });
  if (chunks_.empty()) {
    // producer_finished_ must be true for wait() to have returned here -- nothing left, ever.
    return std::nullopt;
  }
  std::string chunk = std::move(chunks_.front());
  chunks_.pop_front();
  size_bytes_ -= chunk.size();
  lock.unlock();
  not_full_.notify_one();
  return chunk;
}

void AudioBuffer::flush() {
  std::unique_lock<std::mutex> lock(mutex_);
  chunks_.clear();
  size_bytes_ = 0;
  lock.unlock();
  not_full_.notify_all();
}

void AudioBuffer::markProducerFinished() {
  std::unique_lock<std::mutex> lock(mutex_);
  producer_finished_ = true;
  lock.unlock();
  not_empty_.notify_all();
}

void AudioBuffer::notifyConsumerStopped() {
  std::unique_lock<std::mutex> lock(mutex_);
  consumer_stopped_ = true;
  lock.unlock();
  not_full_.notify_all();
}
