#include "../Jobs.h"
#include "tarch/multicore/Core.h"

#if defined(SharedTBB)

#include "tarch/logging/Log.h"
#include "tarch/Assertions.h"
#include "tarch/multicore/tbb/Jobs.h"


#include <vector>
#include <limits>


tarch::logging::Log tarch::multicore::jobs::internal::_log( "tarch::multicore::jobs::internal" );


tbb::atomic<int>                                               tarch::multicore::jobs::internal::_numberOfRunningBackgroundJobConsumerTasks(0);
tbb::concurrent_queue<tarch::multicore::jobs::BackgroundJob*>  tarch::multicore::jobs::internal::_backgroundJobs;
tarch::multicore::jobs::internal::JobMap                       tarch::multicore::jobs::internal::_pendingJobs;
tbb::task_group_context                                        tarch::multicore::jobs::internal::BackgroundJobConsumerTask::backgroundTaskContext;



void tarch::multicore::jobs::terminateAllPendingBackgroundConsumerJobs() {
  internal::BackgroundJobConsumerTask::backgroundTaskContext.cancel_group_execution();
}


void tarch::multicore::jobs::spawnBackgroundJob(BackgroundJob* job) {
  BackgroundJobType mode = job->getJobType();

  switch (mode) {
    case BackgroundJobType::ProcessImmediately:
      job->run();
      delete job;
      break;
    case BackgroundJobType::IsTaskAndRunAsSoonAsPossible:
      {
        internal::TBBBackgroundJobWrapper* tbbTask = new(tbb::task::allocate_root()) internal::TBBBackgroundJobWrapper(job);
        tbb::task::spawn(*tbbTask);
      }
      break;
    case BackgroundJobType::BackgroundJob:
      {
        internal::_backgroundJobs.push(job);
        
        const int currentlyRunningBackgroundThreads = internal::_numberOfRunningBackgroundJobConsumerTasks;
        if (
          currentlyRunningBackgroundThreads<BackgroundJob::_maxNumberOfRunningBackgroundThreads
        ) {
          logDebug( "kickOffBackgroundTask(BackgroundTask*)", "no consumer task running yet or long-running task dropped in; kick off" );
          internal::BackgroundJobConsumerTask::enqueue();
        }
      }
      break;
    case BackgroundJobType::LongRunningBackgroundJob:
      {
        internal::_backgroundJobs.push(job);
        internal::BackgroundJobConsumerTask::enqueue();
      }
      break;
  }
}


/**
 * This routine is typically invoked by user codes to ensure that all
 * background jobs have finished before the user code continues. We have however
 * to take into account that some background jobs might reschedule themselves
 * again as they are persistent. Therefore, we quickly check how many jobs are
 * still pending. Then we add the number of running background jobs (as those
 * guys might reschedule themselves again, so we try to be on the same side).
 * Finally, we process that many jobs that are in the queue and tell the
 * calling routine whether we've done any.
 */
bool tarch::multicore::jobs::processBackgroundJobs() {
  const int numberOfBackgroundJobs = internal::_backgroundJobs.unsafe_size() + internal::_numberOfRunningBackgroundJobConsumerTasks + 1;
  return internal::processNumberOfBackgroundJobs(numberOfBackgroundJobs);
}


int tarch::multicore::jobs::getNumberOfWaitingBackgroundJobs() {
  return internal::_backgroundJobs.unsafe_size();
}


/**
 * Spawn means a thread fires a new job and wants to continue itself.
 *
 * <h2> The spawned job is a task </h2>
 *
 * That means that the new job has no dependencies on any other job. It is
 * thus convenient to launch a real TBB task for it.
 *
 * <h2> The spawned job is not a task </h2>
 *
 * We enqueue it. We may not immediately spawn a job consumer task, as this
 * might mean that TBB might immediately start to consume the job and halt the
 * current thread. This is not what we want: We want to continue with the
 * calling thread immediately.
 */
void tarch::multicore::jobs::spawn(Job*  job) {
  if ( job->isTask() ) {
    logDebug( "spawn(Job*)", "job is a task, so issue TBB task immediately that handles job" );
    internal::TBBJobWrapper* tbbTask = new(tbb::task::allocate_root()) internal::TBBJobWrapper(job);
    tbb::task::spawn(*tbbTask);
  }
  else {
    internal::getJobQueue(job->getClass()).jobs.push(job);

    logDebug( "spawn(Job*)", "enqueued job of class " << job->getClass() );
  }
}


void tarch::multicore::jobs::spawn(std::function<void()>& job, bool isTask, int jobClass) {
  spawn( new tarch::multicore::jobs::GenericJobWithCopyOfFunctor(job,isTask,jobClass) );
}


/**
 * @see processJobs()
 */
int tarch::multicore::jobs::getNumberOfPendingJobs() {
  int result = 0;
  logDebug( "processJobs()", "there are " << _pendingJobs.size() << " class queues" );
  for (auto& p: internal::_pendingJobs) {
	result += p.second.jobs.unsafe_size();
  }
  return result;
}


bool tarch::multicore::jobs::processJobs(int jobClass) {
  logDebug( "processJobs()", "search for jobs of class " << jobClass );

  Job* myTask   = nullptr;
  bool gotOne   = internal::getJobQueue(jobClass).jobs.try_pop(myTask);
  bool result   = false;
  while (gotOne) {
    result   = true;
    logDebug( "processJob(int)", "start to process job of class " << jobClass );
    myTask->run();
    delete myTask;
    logDebug(
      "processJob(int)", "job of class " << jobClass << " complete, there are still " <<
	  getJobQueue(jobClass).jobs.unsafe_size() <<
	  " jobs of this class pending"
	);
    gotOne = internal::getJobQueue(jobClass).jobs.try_pop(myTask);
  }

  return result;
}


/**
 * Work way through the individual queues. Ensure that queues in turn do not 
 * invoke processJobs() again, i.e. pass in false as argument, as we otherwise
 * obtain endless cascadic recursion. The routine should not spawn new tasks on
 * its own, as it is itself used by the job consumer tasks.
 *
 * <h2> Implementation </h2>
 *
 * It is absolutely essential that one uses auto&. With a copy/read-only
 * reference, the code crashes if someone insert stuff concurrently.
 *
 * <h2> Danger </h2>
 *
 * If you run with many job classes, i.e. 'tasks' that depend on each other, then
 * invoking this routine is dangerous. It bears the risk that you spawn more and
 * more jobs that depend on another job and you thus run into a situation, where
 * all TBB tasks process one particular job type.
 */
bool tarch::multicore::jobs::processJobs() {
  bool result = false;

  for (auto& p: internal::_pendingJobs) {
	result |= processJobs(p.first);
  }
  
  return result;
}




/**
 * @see spawnBlockingJob
 */
void tarch::multicore::jobs::spawnAndWait(
  std::function<void()>&  job0,
  std::function<void()>&  job1,  
  bool                    isTask0,
  bool                    isTask1,
  int                     jobClass0,
  int                     jobClass1
) {
  tbb::atomic<int>  semaphore(2);
  
  tbb::parallel_invoke(
    [&] () -> void {
	  internal::spawnBlockingJob( job0, semaphore, isTask0, jobClass0 );
    },
    [&] () -> void {
    	internal::spawnBlockingJob( job1, semaphore, isTask1, jobClass1 );
    }
  );


  while (semaphore>0) {
    tbb::parallel_invoke(
      [&] () -> void {
        processJobs(jobClass0);
      },
      [&] () -> void {
        processJobs(jobClass1);
      }
    );
  }
}



/**
 * @see spawnBlockingJob
 */
void tarch::multicore::jobs::spawnAndWait(
  std::function<void()>&  job0,
  std::function<void()>&  job1,
  std::function<void()>&  job2,
  bool                    isTask0,
  bool                    isTask1,
  bool                    isTask2,
  int                     jobClass0,
  int                     jobClass1,
  int                     jobClass2
) {
  tbb::atomic<int>  semaphore(3);

  tbb::parallel_invoke(
    [&] () -> void {
	  internal::spawnBlockingJob( job0, semaphore, isTask0, jobClass0 );
    },
    [&] () -> void {
    	internal::spawnBlockingJob( job1, semaphore, isTask1, jobClass1 );
    },
    [&] () -> void {
    	internal::spawnBlockingJob( job2, semaphore, isTask2, jobClass2 );
    }
  );

  while (semaphore>0) {
    tbb::parallel_invoke(
      [&] () -> void {
        processJobs(jobClass0);
      },
      [&] () -> void {
        processJobs(jobClass1);
      },
      [&] () -> void {
        processJobs(jobClass2);
      }
    );
  }
}


/**
 * @see spawnBlockingJob
 */
void tarch::multicore::jobs::spawnAndWait(
  std::function<void()>&  job0,
  std::function<void()>&  job1,
  std::function<void()>&  job2,
  std::function<void()>&  job3,
  bool                    isTask0,
  bool                    isTask1,
  bool                    isTask2,
  bool                    isTask3,
  int                     jobClass0,
  int                     jobClass1,
  int                     jobClass2,
  int                     jobClass3
) {
  tbb::atomic<int>  semaphore(4);
  
  tbb::parallel_invoke(
    [&] () -> void {
	  internal::spawnBlockingJob( job0, semaphore, isTask0, jobClass0 );
    },
    [&] () -> void {
    	internal::spawnBlockingJob( job1, semaphore, isTask1, jobClass1 );
    },
    [&] () -> void {
    	internal::spawnBlockingJob( job2, semaphore, isTask2, jobClass2 );
    },
    [&] () -> void {
    	internal::spawnBlockingJob( job3, semaphore, isTask3, jobClass3 );
    }
  );
  
  while (semaphore>0) {
    tbb::parallel_invoke(
      [&] () -> void {
        processJobs(jobClass0);
      },
      [&] () -> void {
        processJobs(jobClass1);
      },
      [&] () -> void {
        processJobs(jobClass2);
      },
      [&] () -> void {
        processJobs(jobClass3);
      }
    );
  }
}



/**
 * @see spawnBlockingJob
 */
void tarch::multicore::jobs::spawnAndWait(
  std::function<void()>& job0,
  std::function<void()>& job1,
  std::function<void()>& job2,
  std::function<void()>& job3,
  std::function<void()>& job4,
	 bool                    isTask0,
	 bool                    isTask1,
	 bool                    isTask2,
	 bool                    isTask3,
	 bool                    isTask4,
	 int                     jobClass0,
	 int                     jobClass1,
	 int                     jobClass2,
	 int                     jobClass3,
	 int                     jobClass4
) {
  tbb::atomic<int>  semaphore(5);
  
  tbb::parallel_invoke(
    [&] () -> void {
	  internal::spawnBlockingJob( job0, semaphore, isTask0, jobClass0 );
    },
    [&] () -> void {
    	internal::spawnBlockingJob( job1, semaphore, isTask1, jobClass1 );
    },
    [&] () -> void {
    	internal::spawnBlockingJob( job2, semaphore, isTask2, jobClass2 );
    },
    [&] () -> void {
    	internal::spawnBlockingJob( job3, semaphore, isTask3, jobClass3 );
    },
    [&] () -> void {
    	internal::spawnBlockingJob( job4, semaphore, isTask4, jobClass4 );
    }
  );
  

  while (semaphore>0) {
    tbb::parallel_invoke(
      [&] () -> void {
        processJobs(jobClass0);
      },
      [&] () -> void {
        processJobs(jobClass1);
      },
      [&] () -> void {
        processJobs(jobClass2);
      },
      [&] () -> void {
        processJobs(jobClass3);
      },
      [&] () -> void {
        processJobs(jobClass4);
      }
    );
  }
}



/**
 * @see spawnBlockingJob
 */
void tarch::multicore::jobs::spawnAndWait(
  std::function<void()>&  job0,
  std::function<void()>&  job1,
  std::function<void()>&  job2,
  std::function<void()>&  job3,
  std::function<void()>&  job4,
  std::function<void()>&  job5,
  bool                    isTask0,
  bool                    isTask1,
  bool                    isTask2,
  bool                    isTask3,
  bool                    isTask4,
  bool                    isTask5,
  int                     jobClass0,
  int                     jobClass1,
  int                     jobClass2,
  int                     jobClass3,
  int                     jobClass4,
  int                     jobClass5
) {
  tbb::atomic<int>  semaphore(6);
  
  tbb::parallel_invoke(
    [&] () -> void {
	  internal::spawnBlockingJob( job0, semaphore, isTask0, jobClass0 );
    },
    [&] () -> void {
    	internal::spawnBlockingJob( job1, semaphore, isTask1, jobClass1 );
    },
    [&] () -> void {
    	internal::spawnBlockingJob( job2, semaphore, isTask2, jobClass2 );
    },
    [&] () -> void {
    	internal::spawnBlockingJob( job3, semaphore, isTask3, jobClass3 );
    },
    [&] () -> void {
    	internal::spawnBlockingJob( job4, semaphore, isTask4, jobClass4 );
    },
    [&] () -> void {
      internal::spawnBlockingJob( job5, semaphore, isTask5, jobClass5 );
    }
  );
  
  while (semaphore>0) {
    tbb::parallel_invoke(
      [&] () -> void {
        processJobs(jobClass0);
      },
      [&] () -> void {
        processJobs(jobClass1);
      },
      [&] () -> void {
        processJobs(jobClass2);
      },
      [&] () -> void {
        processJobs(jobClass3);
      },
      [&] () -> void {
        processJobs(jobClass4);
      },
      [&] () -> void {
        processJobs(jobClass5);
      }
    );
  }
}


#endif
