#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>


// Class that represents a simple thread pool
class ThreadPool {
public:
    // // Constructor to creates a thread pool with given
    // number of threads
    ThreadPool(size_t num_threads = 0) {
        if (num_threads == 0)
            num_threads = std::thread::hardware_concurrency();

        // Creating worker threads
        for (size_t i = 0; i < num_threads; ++i) {
            threads.emplace_back([this] {
                while (true) {
                    std::function<bool()> task;

                    // The reason for putting the below code
                    // here is to unlock the queue before
                    // executing the task so that other
                    // threads can perform enqueue tasks
                    {
                        // Locking the queue so that data
                        // can be shared safely
                        std::unique_lock<std::mutex> lock(mutex);

                        // Waiting until there is a task to
                        // execute or the pool is stopped
                        condition.wait(lock, [this] {
                            return !tasks.empty() || stop;
                        });

                        // exit the thread in case the pool
                        // is stopped and there are no tasks
                        if (stop && tasks.empty()) {
                            return;
                        }

                        // Get the next task from the queue
                        task = std::move(tasks.front());
                        tasks.pop();
                        number_working++;
                    }

                    bool error = task();

                    // Stop if one of the tasks has an error.
                    {
                        std::unique_lock<std::mutex> lock(mutex);
                        number_working--;
                        if (error) {
                            got_error = true;
                            stop = true;
                        }
                    }

                    // Notify others that the task has been finished.
                    // or that we should stop.
                    condition.notify_all();
                }
            });
        }
    }

    // Destructor to stop the thread pool
    ~ThreadPool() {
        {
            // Lock the queue to update the stop flag safely
            std::unique_lock<std::mutex> lock(mutex);
            stop = true;
        }

        // Notify all threads
        condition.notify_all();

        // Joining all worker threads to ensure they have
        // completed their tasks
        for (auto& thread : threads) {
            thread.join();
        }
    }

    void join() {
        {
            // Waiting until there are no tasks to execute anymore, and then stop.
            std::unique_lock<std::mutex> lock(mutex);
            condition.wait(lock, [this] {
                return (tasks.empty() && number_working == 0) || stop;
            });

            stop = true;
        }

        condition.notify_all();
        for (auto& thread : threads)
            thread.join();
        threads.clear();
    }

    // Enqueue task for execution by the thread pool
    void enqueue(std::function<bool()> task) {
        {
            std::unique_lock<std::mutex> lock(mutex);
            tasks.emplace(std::move(task));
        }
        condition.notify_all();
    }

    bool got_error = false;

private:
    // Vector to store worker threads
    std::vector<std::thread> threads;

    // Queue of tasks
    std::queue<std::function<bool()> > tasks;

    // Mutex to synchronize access to shared data
    std::mutex mutex;

    // Condition variable to signal changes in the state of
    // the tasks queue
    std::condition_variable condition;

    // Flag to indicate whether the thread pool should stop
    // or not
    bool stop = false;

    //
    int number_working = 0;
};
