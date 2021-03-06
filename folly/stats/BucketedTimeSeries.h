/*
 * Copyright 2012 Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef FOLLY_STATS_BUCKETEDTIMESERIES_H_
#define FOLLY_STATS_BUCKETEDTIMESERIES_H_

#include <chrono>
#include <vector>

#include "folly/detail/Stats.h"

namespace folly {

/*
 * This class represents a bucketed time series which keeps track of values
 * added in the recent past, and merges these values together into a fixed
 * number of buckets to keep a lid on memory use if the number of values
 * added is very large.
 *
 * For example, a BucketedTimeSeries() with duration == 60s and 10 buckets
 * will keep track of 10 6-second buckets, and discard all data added more
 * than 1 minute ago.  As time ticks by, a 6-second bucket at a time will
 * be discarded and new data will go into the newly opened bucket.  Internally,
 * it uses a circular array of buckets that it reuses as time advances.
 *
 * The class assumes that time advances forward --  you can't retroactively add
 * values for events in the past -- the 'now' argument is provided for better
 * efficiency and ease of unittesting.
 *
 *
 * This class is not thread-safe -- use your own synchronization!
 */
template <typename VT, typename TT=std::chrono::seconds>
class BucketedTimeSeries {
 public:
  typedef VT ValueType;
  typedef TT TimeType;
  typedef detail::Bucket<ValueType> Bucket;

  /*
   * Create a new BucketedTimeSeries.
   *
   * This creates a new BucketedTimeSeries with the specified number of
   * buckets, storing data for the specified amount of time.
   *
   * If the duration is 0, the BucketedTimeSeries will track data forever,
   * and does not need the rolling buckets.  The numBuckets parameter is
   * ignored when duration is 0.
   */
  BucketedTimeSeries(size_t numBuckets, TimeType duration);

  /*
   * Adds the value 'val' at time 'now'
   *
   * This function expects time to always move forwards: it cannot be used to
   * add historical data points that have occurred in the past.  If now is
   * older than the another timestamp that has already been passed to
   * addValue() or update(), now will be ignored and the latest timestamp will
   * be used.
   */
  void addValue(TimeType now, const ValueType& val);

  /*
   * Adds the value 'val' the given number of 'times' at time 'now'
   */
  void addValue(TimeType now, const ValueType& val, int64_t times);

  /*
   * Adds the value 'sum' as the sum of 'nsamples' samples
   */
  void addValueAggregated(TimeType now, const ValueType& sum, int64_t nsamples);

  /*
   * Updates the container to the specified time, doing all the necessary
   * work to rotate the buckets and remove any stale data points.
   *
   * The addValue() methods automatically call update() when adding new data
   * points.  However, when reading data from the timeseries, you should make
   * sure to manually call update() before accessing the data.  Otherwise you
   * may be reading stale data if update() has not been called recently.
   *
   * Returns the current bucket index after the update.
   */
  size_t update(TimeType now);

  /*
   * Reset the timeseries to an empty state,
   * as if no data points have ever been added to it.
   */
  void clear();

  /*
   * Get the latest time that has ever been passed to update() or addValue().
   */
  TimeType getLatestTime() const {
    return latestTime_;
  }

  /*
   * Return the number of buckets.
   */
  size_t numBuckets() const {
    return buckets_.size();
  }

  /*
   * Return the maximum duration of data that can be tracked by this
   * BucketedTimeSeries.
   */
  TimeType duration() const {
    return duration_;
  }

  /*
   * Returns true if this BucketedTimeSeries stores data for all-time, without
   * ever rolling over into new buckets.
   */
  bool isAllTime() const {
    return (duration_ == TimeType(0));
  }

  /*
   * Returns true if no calls to update() have been made since the last call to
   * clear().
   */
  bool empty() const {
    // We set firstTime_ greater than latestTime_ in the constructor and in
    // clear, so we use this to distinguish if the timeseries is empty.
    //
    // Once a data point has been added, latestTime_ will always be greater
    // than or equal to firstTime_.
    return firstTime_ > latestTime_;
  }

  /*
   * Get the amount of time tracked by this timeseries.
   *
   * For an all-time timeseries, this returns the length of time since the
   * first data point was added to the time series.
   *
   * Otherwise, this never returns a value greater than the overall timeseries
   * duration.  If the first data point was recorded less than a full duration
   * ago, the time since the first data point is returned.  If a full duration
   * has elapsed, and we have already thrown away some data, the time since the
   * oldest bucket is returned.
   *
   * For example, say we are tracking 600 seconds worth of data, in 60 buckets.
   * - If less than 600 seconds have elapsed since the first data point,
   *   elapsed() returns the total elapsed time so far.
   * - If more than 600 seconds have elapsed, we have already thrown away some
   *   data.  However, we throw away a full bucket (10 seconds worth) at once,
   *   so at any point in time we have from 590 to 600 seconds worth of data.
   *   elapsed() will therefore return a value between 590 and 600.
   *
   * Note that you generally should call update() before calling elapsed(), to
   * make sure you are not reading stale data.
   */
  TimeType elapsed() const;

  /*
   * Return the sum of all the data points currently tracked by this
   * BucketedTimeSeries.
   *
   * Note that you generally should call update() before calling sum(), to
   * make sure you are not reading stale data.
   */
  const ValueType& sum() const {
    return total_.sum;
  }

  /*
   * Return the number of data points currently tracked by this
   * BucketedTimeSeries.
   *
   * Note that you generally should call update() before calling count(), to
   * make sure you are not reading stale data.
   */
  uint64_t count() const {
    return total_.count;
  }

  /*
   * Return the average value (sum / count).
   *
   * The return type may be specified to control whether floating-point or
   * integer division should be performed.
   *
   * Note that you generally should call update() before calling avg(), to
   * make sure you are not reading stale data.
   */
  template <typename ReturnType=double>
  ReturnType avg() const {
    return total_.template avg<ReturnType>();
  }

  /*
   * Return the sum divided by the elapsed time.
   *
   * Note that you generally should call update() before calling rate(), to
   * make sure you are not reading stale data.
   */
  template <typename ReturnType=double, typename Interval=TimeType>
  ReturnType rate() const {
    return rateHelper<ReturnType, Interval>(total_.sum, elapsed());
  }

  /*
   * Return the count divided by the elapsed time.
   *
   * The Interval template parameter causes the elapsed time to be converted to
   * the Interval type before using it.  For example, if Interval is
   * std::chrono::seconds, the return value will be the count per second.
   * If Interval is std::chrono::hours, the return value will be the count per
   * hour.
   *
   * Note that you generally should call update() before calling countRate(),
   * to make sure you are not reading stale data.
   */
  template <typename ReturnType=double, typename Interval=TimeType>
  ReturnType countRate() const {
    return rateHelper<ReturnType, Interval>(total_.count, elapsed());
  }

  /*
   * Estimate the sum of the data points that occurred in the specified time
   * period.
   *
   * The range queried is [start, end).
   * That is, start is inclusive, and end is exclusive.
   *
   * Note that data outside of the timeseries duration will no longer be
   * available for use in the estimation.  Specifying a start time earlier than
   * (getLatestTime() - elapsed()) will not have much effect, since only data
   * points after that point in time will be counted.
   *
   * Note that the value returned is an estimate, and may not be precise.
   */
  ValueType sum(TimeType start, TimeType end) const;

  /*
   * Estimate the number of data points that occurred in the specified time
   * period.
   *
   * The same caveats documented in the sum(TimeType start, TimeType end)
   * comments apply here as well.
   */
  uint64_t count(TimeType start, TimeType end) const;

  /*
   * Estimate the average value during the specified time period.
   *
   * The same caveats documented in the sum(TimeType start, TimeType end)
   * comments apply here as well.
   */
  template <typename ReturnType=double>
  ReturnType avg(TimeType start, TimeType end) const;

  /*
   * Estimate the rate during the specified time period.
   *
   * The same caveats documented in the sum(TimeType start, TimeType end)
   * comments apply here as well.
   */
  template <typename ReturnType=double, typename Interval=TimeType>
  ReturnType rate(TimeType start, TimeType end) const {
    ValueType intervalSum = sum(start, end);
    return rateHelper<ReturnType, Interval>(intervalSum, end - start);
  }

  /*
   * Estimate the rate of data points being added during the specified time
   * period.
   *
   * The same caveats documented in the sum(TimeType start, TimeType end)
   * comments apply here as well.
   */
  template <typename ReturnType=double, typename Interval=TimeType>
  ReturnType countRate(TimeType start, TimeType end) const {
    uint64_t intervalCount = count(start, end);
    return rateHelper<ReturnType, Interval>(intervalCount, end - start);
  }

  /*
   * Invoke a function for each bucket.
   *
   * The function will take as arguments the bucket index,
   * the bucket start time, and the start time of the subsequent bucket.
   *
   * It should return true to continue iterating through the buckets, and false
   * to break out of the loop and stop, without calling the function on any
   * more buckets.
   *
   * bool function(const Bucket& bucket, TimeType bucketStart,
   *               TimeType nextBucketStart)
   */
  template <typename Function>
  void forEachBucket(Function fn) const;

  /*
   * Get the index for the bucket containing the specified time.
   *
   * Note that the index is only valid if this time actually falls within one
   * of the current buckets.  If you pass in a value more recent than
   * getLatestTime() or older than (getLatestTime() - elapsed()), the index
   * returned will not be valid.
   *
   * This method may not be called for all-time data.
   */
  size_t getBucketIdx(TimeType time) const;

  /*
   * Get the bucket at the specified index.
   *
   * This method may not be called for all-time data.
   */
  const Bucket& getBucketByIndex(size_t idx) const {
    return buckets_[idx];
  }

  /*
   * Compute the bucket index that the specified time falls into,
   * as well as the bucket start time and the next bucket's start time.
   *
   * This method may not be called for all-time data.
   */
  void getBucketInfo(TimeType time, size_t* bucketIdx,
                     TimeType* bucketStart, TimeType* nextBucketStart) const;

 private:
  template <typename ReturnType=double, typename Interval=TimeType>
  ReturnType rateHelper(ReturnType numerator, TimeType elapsed) const {
    if (elapsed == TimeType(0)) {
      return 0;
    }

    // Use std::chrono::duration_cast to convert between the native
    // duration and the desired interval.  However, convert the rates,
    // rather than just converting the elapsed duration.  Converting the
    // elapsed time first may collapse it down to 0 if the elapsed interval
    // is less than the desired interval, which will incorrectly result in
    // an infinite rate.
    typedef std::chrono::duration<
        ReturnType, std::ratio<TimeType::period::den,
                               TimeType::period::num>> NativeRate;
    typedef std::chrono::duration<
        ReturnType, std::ratio<Interval::period::den,
                               Interval::period::num>> DesiredRate;

    NativeRate native(numerator / elapsed.count());
    DesiredRate desired = std::chrono::duration_cast<DesiredRate>(native);
    return desired.count();
  }

  ValueType rangeAdjust(TimeType bucketStart, TimeType nextBucketStart,
                        TimeType start, TimeType end,
                        ValueType input) const;

  template <typename Function>
  void forEachBucket(TimeType start, TimeType end, Function fn) const;

  TimeType firstTime_;   // time of first update() since clear()/constructor
  TimeType latestTime_;  // time of last update()
  TimeType duration_;    // total duration ("window length") of the time series

  Bucket total_;                 // sum and count of everything in time series
  std::vector<Bucket> buckets_;  // actual buckets of values
};

} // folly

#endif // FOLLY_STATS_BUCKETEDTIMESERIES_H_
