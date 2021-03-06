// This file is part of the Peano project. For conditions of distribution and
// use, please see the copyright notice at www.peano-framework.org
#ifndef _PEANO_HEAP_ALIGNED_CHAR_SEND_RECEIVE_TASK_H_
#define _PEANO_HEAP_ALIGNED_CHAR_SEND_RECEIVE_TASK_H_


#include "peano/utils/Globals.h"
#include "peano/utils/PeanoOptimisations.h"
#include "peano/heap/records/MetaInformation.h"
#include "peano/heap/HeapAllocator.h"


#include <vector>

#ifdef Parallel
#include <mpi.h>
#endif


namespace peano {
  namespace heap {
    template<int Alignment>
    struct AlignedCharSendReceiveTask;
  }
}


/**
 * Wrapper for a send or receive task
 *
 * Holds the information for a send or receive task, i.e. it holds the MPI
 * request to check weather the task has been finished (both sends and
 * receives are realised as non-blocking MPI data exchange) as well as a
 * pointer to the allocated data where the copy of the data is
 * stored (send task) or is to be stored (receive task).
 */
template<int Alignment>
struct peano::heap::AlignedCharSendReceiveTask {
  typedef std::vector< char, HeapAllocator<char, Alignment > >  DataVectorType;


  static tarch::logging::Log _log;


  /**
   * We always use the plain meta information as record, i.e. we do not pack
   * anything here as the meta information usually is one integer only
   * anyway.
   */
  typedef peano::heap::records::MetaInformation          MetaInformation;

  #ifdef Parallel
  MPI_Request     _request;
  #endif

  MetaInformation _metaInformation;

  /**
   * Without semantics for send tasks but important for receive tasks as we
   * have to store from which rank the data arrived from.
   */
  int             _rank;

  /**
   * Pointer to the actual data. If meta data marks a message without
   * content, this pointer is 0.
   */
  char*           _data;

  bool            _freeDataPointer;

  #ifdef Asserts
  AlignedCharSendReceiveTask();
  #endif

  /**
   * Prelude to sendData().
   *
   * Please note that you have to call delete[] on _data afterwards through
   * operation freeMemory().
   */
  void wrapData(const char* const data);

  /**
   * Counterpart of wrapData(). The task sends away the data directly from the
   * specified buffer. Please call freeMemory() nevertheless.
   */
  void sendDataDirectlyFromBuffer(const char* const data);

  /**
   * @see triggerReceive() for implementation remarks.
   */
  void triggerSend(int tag);

  /**
   * If you use the task in combination with containers, please push or pop
   * data first and then call trigger. Trigger uses MPI and MPI uses memory
   * references. If you move the tasks around later on, MPI references invalid
   * memory addresses and data consistency is not given anymore.
   */
  void triggerReceive(int tag);

  void freeMemory();

  /**
   * Set a task invalid explicitly. Messages marked that way will pass the
   * validation though their data is not in agreement with checks: it is
   * explicilty known that the message is invalid and can be ignored. I use
   * this for null messages, i.e. messages without content that are often
   * squeezed (together with their meta data) by sophisticated communication
   * schemes.
   */
  void setInvalid();

  /**
   * A task fits if it is
   *
   * - either invalid (see setInvalid())
   * - or position and level coincide.
   *
   * Fits should only be called in assert mode. However, some compiler seem to
   * translate it also if the function is not used at all. For them, I provide
   * a non-asserts version returning true all the time.
   */
  bool fits(
    const tarch::la::Vector<DIMENSIONS, double>&  position,
    int                                           level
  ) const;

  std::string toString() const;
};


#include "peano/heap/AlignedCharSendReceiveTask.cpph"


#endif
