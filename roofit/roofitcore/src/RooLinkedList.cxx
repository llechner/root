/*****************************************************************************
 * Project: RooFit                                                           *
 * Package: RooFitCore                                                       *
 * @(#)root/roofitcore:$Id$
 * Authors:                                                                  *
 *   WV, Wouter Verkerke, UC Santa Barbara, verkerke@slac.stanford.edu       *
 *   DK, David Kirkby,    UC Irvine,         dkirkby@uci.edu                 *
 *                                                                           *
 * Copyright (c) 2000-2005, Regents of the University of California          *
 *                          and Stanford University. All rights reserved.    *
 *                                                                           *
 * Redistribution and use in source and binary forms,                        *
 * with or without modification, are permitted according to the terms        *
 * listed in LICENSE (http://roofit.sourceforge.net/license.txt)             *
 *****************************************************************************/

//////////////////////////////////////////////////////////////////////////////
//
// BEGIN_HTML
// RooLinkedList is an collection class for internal use, storing
// a collection of RooAbsArg pointers in a doubly linked list.
// It can optionally add a hash table to speed up random access
// in large collections
// Use RooAbsCollection derived objects for public use
// (e.g. RooArgSet or RooArgList) 
// END_HTML
//

#include <algorithm>

#include "RooFit.h"
#include "Riostream.h"

#include "RooLinkedList.h"
#include "RooLinkedListIter.h"
#include "RooHashTable.h"
#include "RooAbsArg.h"
#include "RooMsgService.h"

using namespace std;

ClassImp(RooLinkedList)
;
namespace RooLinkedListImplDetails {
  /// a chunk of memory in a pool for quick allocation of RooLinkedListElems
  class Chunk {
    public:
      /// constructor
      Chunk(Int_t sz) :
	_sz(sz), _free(capacity()),
	_chunk(new RooLinkedListElem[_free]), _freelist(_chunk)
      {
	// initialise free list
	for (Int_t i = 0; i < _free; ++i)
	  _chunk[i]._next = (i + 1 < _free) ? &_chunk[i + 1] : 0;
      }
      /// destructor
      ~Chunk() { delete[] _chunk; }
      /// chunk capacity
      Int_t capacity() const
      { return (1 << _sz) / sizeof(RooLinkedListElem); }
      /// chunk free elements
      Int_t free() const { return _free; }
      /// chunk occupied elements
      Int_t size() const { return capacity() - free(); }
      /// return size class
      int szclass() const { return _sz; }
      /// chunk full?
      bool full() const { return !free(); }
      /// chunk empty?
      bool empty() const { return capacity() == free(); }
      /// return address of chunk
      const void* chunkaddr() const { return _chunk; }
      /// check if el is in this chunk
      bool contains(RooLinkedListElem* el) const
      { return _chunk <= el && el < &_chunk[capacity()]; }
      /// pop a free element off the free list
      RooLinkedListElem* pop_free_elem()
      {
	if (!_freelist) return 0;
	RooLinkedListElem* retVal = _freelist;
	_freelist = retVal->_next;
	retVal->_arg = 0; retVal->_refCount = 0;
	retVal->_prev = retVal->_next = 0;
	--_free;
	return retVal;
      }
      /// push a free element back onto the freelist
      void push_free_elem(RooLinkedListElem* el)
      {
	el->_next = _freelist;
	_freelist = el;
	++_free;
      }
    private:
      Int_t _sz;				///< chunk capacity
      Int_t _free;			///< length of free list
      RooLinkedListElem* _chunk;		///< chunk from which elements come
      RooLinkedListElem* _freelist;	///< list of free elements

      /// forbid copying
      Chunk(const Chunk&);
      // forbid assignment
      Chunk& operator=(const Chunk&);
  };

  class Pool {
    private:
      enum {
	minsz = 7, ///< minimum chunk size (just below 1 << minsz bytes)
	maxsz = 20, ///< maximum chunk size (just below 1 << maxsz bytes)
	szincr = 1 ///< size class increment (sz = 1 << (minsz + k * szincr))
      };
      /// a chunk of memory in the pool
      typedef RooLinkedListImplDetails::Chunk Chunk;
      typedef std::list<Chunk*> ChunkList;
      typedef std::map<const void*, Chunk*> AddrMap;
    public:
      /// constructor
      Pool();
      /// destructor
      ~Pool();
      /// acquire the pool
      inline void acquire() { ++_refCount; }
      /// release the pool, return true if the pool is unused
      inline bool release() { return 0 == --_refCount; }
      /// pop a free element out of the pool
      RooLinkedListElem* pop_free_elem();
      /// push a free element back into the pool
      void push_free_elem(RooLinkedListElem* el);
    private:
      AddrMap _addrmap;
      ChunkList _freelist;
      UInt_t _szmap[(maxsz - minsz) / szincr];
      Int_t _cursz;
      UInt_t _refCount;

      /// adjust _cursz to current largest block
      void updateCurSz(Int_t sz, Int_t incr);
      /// find size of next chunk to allocate (in a hopefully smart way)
      Int_t nextChunkSz() const;
  };
  
  Pool::Pool() : _cursz(minsz), _refCount(0)
  {
    std::fill(_szmap, _szmap + ((maxsz - minsz) / szincr), 0);
  }

  Pool::~Pool()
  {
    _freelist.clear();
    for (AddrMap::iterator it = _addrmap.begin(); _addrmap.end() != it; ++it)
      delete it->second;
    _addrmap.clear();
  }

  RooLinkedListElem* Pool::pop_free_elem()
  {
    if (_freelist.empty()) {
      // allocate and register new chunk and put it on the freelist
      const Int_t sz = nextChunkSz();
      Chunk *c = new Chunk(sz);
      _addrmap[c->chunkaddr()] = c;
      _freelist.push_back(c);
      updateCurSz(sz, +1);
    }
    // get free element from first chunk on _freelist
    Chunk* c = _freelist.front();
    RooLinkedListElem* retVal = c->pop_free_elem();
    // full chunks are removed from _freelist
    if (c->full()) _freelist.pop_front();
    return retVal;
  }

  void Pool::push_free_elem(RooLinkedListElem* el)
  {
    // find from which chunk el came
    AddrMap::iterator ci = _addrmap.end();
    if (!_addrmap.empty()) {
      ci = _addrmap.lower_bound(el);
      if (ci == _addrmap.end()) {
	// point beyond last element, so get last one
	ci = (++_addrmap.rbegin()).base();
      } else {
	// valid ci, check if we need to decrement ci because el isn't the
	// first element in the chunk
	if (_addrmap.begin() != ci && ci->first != el) --ci;
      }
    }
    // either empty addressmap, or ci should now point to the chunk which might
    // contain el
    if (_addrmap.empty() || !ci->second->contains(el)) {
      // el is not in any chunk we know about, so just delete it
      delete el;
      return;
    }
    Chunk *c = ci->second;
    const bool moveToFreelist = c->full();
    c->push_free_elem(el);
    if (c->empty()) {
      // delete chunk if all empty
      ChunkList::iterator it = std::find( _freelist.begin(), _freelist.end(), c);
      if (_freelist.end() != it) _freelist.erase(it);
      _addrmap.erase(ci->first);
      updateCurSz(c->szclass(), -1);
      delete c;
    } else if (moveToFreelist) {
      _freelist.push_back(c);
    }
  }

  void Pool::updateCurSz(Int_t sz, Int_t incr)
  {
    _szmap[(sz - minsz) / szincr] += incr;
    _cursz = minsz;
    for (int i = (maxsz - minsz) / szincr; i--; ) {
      if (_szmap[i]) {
	_cursz += i * szincr;
	break;
      }
    }
  }

  Int_t Pool::nextChunkSz() const
  {
    // no chunks with space available, figure out chunk size
    Int_t sz = _cursz;
    if (_addrmap.empty()) {
      // if we start allocating chunks, we start from minsz
      sz = minsz;
    } else {
      if (minsz >= sz) {
	// minimal sized chunks are always grown
	sz = minsz + szincr;
      } else {
	if (1 != _addrmap.size()) {
	  // if we have more than one completely filled chunk, grow
	  sz += szincr;
	} else {
	  // just one chunk left, try shrinking chunk size
	  sz -= szincr;
	}
      }
    }
    // clamp size to allowed range
    if (sz > maxsz) sz = maxsz;
    if (sz < minsz) sz = minsz;
    return sz;
  }
}

RooLinkedList::Pool* RooLinkedList::_pool = 0;

//_____________________________________________________________________________
RooLinkedList::RooLinkedList(Int_t htsize) : 
  _hashThresh(htsize), _size(0), _first(0), _last(0), _htableName(0), _htableLink(0), _useNptr(kFALSE)
{
  if (!_pool) _pool = new Pool;
  _pool->acquire();
}

//_____________________________________________________________________________
RooLinkedList::RooLinkedList(const RooLinkedList& other) :
  TObject(other), _hashThresh(other._hashThresh), _size(0), _first(0), _last(0), _htableName(0), _htableLink(0), 
  _name(other._name), 
  _useNptr(other._useNptr)
{
  // Copy constructor
  if (!_pool) _pool = new Pool;
  _pool->acquire();
  if (other._htableName) _htableName = new RooHashTable(other._htableName->size()) ;
  if (other._htableLink) _htableLink = new RooHashTable(other._htableLink->size(),RooHashTable::Pointer) ;
  for (RooLinkedListElem* elem = other._first; elem; elem = elem->_next) {
    Add(elem->_arg, elem->_refCount) ;
  }
}

//_____________________________________________________________________________
RooLinkedListElem* RooLinkedList::createElement(TObject* obj, RooLinkedListElem* elem) 
{
//   cout << "RooLinkedList::createElem(" << this << ") obj = " << obj << " elem = " << elem << endl ;
  RooLinkedListElem* ret = _pool->pop_free_elem();
  ret->init(obj, elem);
  return ret ;
}

//_____________________________________________________________________________
void RooLinkedList::deleteElement(RooLinkedListElem* elem) 
{  
  elem->release() ;
  _pool->push_free_elem(elem);
  //delete elem ;
}

//_____________________________________________________________________________
RooLinkedList& RooLinkedList::operator=(const RooLinkedList& other) 
{
  // Assignment operator, copy contents from 'other'
  
  // Prevent self-assignment
  if (&other==this) return *this ;
  
  // remove old elements
  Clear();
  // Copy elements
  for (RooLinkedListElem* elem = other._first; elem; elem = elem->_next) {
    Add(elem->_arg) ;
  }    
  
  return *this ;
}

//_____________________________________________________________________________
void RooLinkedList::setHashTableSize(Int_t size) 
{        
  // Change the threshold for hash-table use to given size.
  // If a hash table exists when this method is called, it is regenerated.

  if (size<0) {
    coutE(InputArguments) << "RooLinkedList::setHashTable() ERROR size must be positive" << endl ;
    return ;
  }
  if (size==0) {
    if (!_htableName) {
      // No hash table present
      return ;
    } else {
      // Remove existing hash table
      delete _htableName ;
      delete _htableLink ;
      _htableName = 0 ;
      _htableLink = 0 ;
    }
  } else {
    
    // (Re)create hash tables
    if (_htableName) delete _htableName ;
    _htableName = new RooHashTable(size) ;

     if (_htableLink) delete _htableLink ;
     _htableLink = new RooHashTable(size,RooHashTable::Pointer) ;
    
    // Fill hash table with existing entries
    RooLinkedListElem* ptr = _first ;
    while(ptr) {
      _htableName->add(ptr->_arg) ;
      _htableLink->add((TObject*)ptr,ptr->_arg) ;
      ptr = ptr->_next ;
    }      
  }
}

//_____________________________________________________________________________
RooLinkedList::~RooLinkedList() 
{
  // Destructor

  if (_htableName) {
    delete _htableName ;
    _htableName=0 ;
  }
  if (_htableLink) {
    delete _htableLink ;
    _htableLink=0 ;
  }
  
  Clear() ;
  if (_pool->release()) {
    delete _pool;
    _pool = 0;
  }
}

//_____________________________________________________________________________
RooLinkedListElem* RooLinkedList::findLink(const TObject* arg) const 
{    
  // Find the element link containing the given object

  if (_htableLink) {
    return _htableLink->findLinkTo(arg) ;  
  }
  
  RooLinkedListElem* ptr = _first;
  while(ptr) {
    if (ptr->_arg == arg) {
      return ptr ;
    }
    ptr = ptr->_next ;
  }
  return 0 ;
  
}

//_____________________________________________________________________________
void RooLinkedList::Add(TObject* arg, Int_t refCount)
{
  // Insert object into collection with given reference count value

  if (!arg) return ;
  
  // Add to hash table 
  if (_htableName) {

    // Expand capacity of hash table if #entries>#slots
    if (_size > _htableName->size()) {
      setHashTableSize(_size*2) ;
    }

  } else if (_hashThresh>0 && _size>_hashThresh) {

    setHashTableSize(_hashThresh) ;
  }  

  if (_last) {
    // Append element at end of list
    _last = createElement(arg,_last) ;
  } else {
    // Append first element, set first,last 
    _last = createElement(arg) ;
    _first=_last ;
  }

  if (_htableName){
    //cout << "storing link " << _last << " with hash arg " << arg << endl ;
    _htableName->add(arg) ;
    _htableLink->add((TObject*)_last,arg) ;
  }

  _size++ ;
  _last->_refCount = refCount ;
  
}

//_____________________________________________________________________________
Bool_t RooLinkedList::Remove(TObject* arg) 
{
  // Remove object from collection

  // Find link element
  RooLinkedListElem* elem = findLink(arg) ;
  if (!elem) return kFALSE ;
  
  // Remove from hash table
  if (_htableName) {
    _htableName->remove(arg) ;
  }
  if (_htableLink) {
    _htableLink->remove((TObject*)elem,arg) ;
  }
  
  // Update first,last if necessary
  if (elem==_first) _first=elem->_next ;
  if (elem==_last) _last=elem->_prev ;
  
  // Delete and shrink
  _size-- ;
  deleteElement(elem) ;	
  return kTRUE ;
}

//_____________________________________________________________________________
TObject* RooLinkedList::At(Int_t index) const 
{
  // Return object stored in sequential position given by index.
  // If index is out of range, a null pointer is returned.

  // Check range
  if (index<0 || index>=_size) return 0 ;

  
  // Walk list
  RooLinkedListElem* ptr = _first;
  while(index--) ptr = ptr->_next ;
  
  // Return arg
  return ptr->_arg ;
}

//_____________________________________________________________________________
Bool_t RooLinkedList::Replace(const TObject* oldArg, const TObject* newArg) 
{
  // Replace object 'oldArg' in collection with new object 'newArg'.
  // If 'oldArg' is not found in collection kFALSE is returned

  // Find existing element and replace arg
  RooLinkedListElem* elem = findLink(oldArg) ;
  if (!elem) return kFALSE ;
  
  if (_htableName) {
    _htableName->replace(oldArg,newArg) ;
  }
  if (_htableLink) {
    // Link is hashed by contents and may change slot in hash table
    _htableLink->remove((TObject*)elem,(TObject*)oldArg) ;
    _htableLink->add((TObject*)elem,(TObject*)newArg) ;
  }

  elem->_arg = (TObject*)newArg ;
  return kTRUE ;
}

//_____________________________________________________________________________
TObject* RooLinkedList::FindObject(const char* name) const 
{
  // Return pointer to obejct with given name. If no such object
  // is found return a null pointer

  return find(name) ;
}

//_____________________________________________________________________________
TObject* RooLinkedList::FindObject(const TObject* obj) const 
{
  // Find object in list. If list contains object return 
  // (same) pointer to object, otherwise return null pointer

  RooLinkedListElem *elem = findLink((TObject*)obj) ;
  return elem ? elem->_arg : 0 ;
}

//_____________________________________________________________________________
void RooLinkedList::Clear(Option_t *) 
{
  // Remove all elements from collection

  for (RooLinkedListElem *elem = _first, *next; elem; elem = next) {
    next = elem->_next ;
    deleteElement(elem) ;
  }
  _first = 0 ;
  _last = 0 ;
  _size = 0 ;
  
  if (_htableName) {
    Int_t hsize = _htableName->size() ;
    delete _htableName ;
    _htableName = new RooHashTable(hsize) ;   
  }
  if (_htableLink) {
    Int_t hsize = _htableLink->size() ;
    delete _htableLink ;
    _htableLink = new RooHashTable(hsize,RooHashTable::Pointer) ;       
  }
}

//_____________________________________________________________________________
void RooLinkedList::Delete(Option_t *) 
{
  // Remove all elements in collection and delete all elements
  // NB: Collection does not own elements, this function should
  // be used judiciously by caller. 

  RooLinkedListElem* elem = _first;
  while(elem) {
    RooLinkedListElem* next = elem->_next ;
    delete elem->_arg ;
    deleteElement(elem) ;
    elem = next ;
  }
  _first = 0 ;
  _last = 0 ;
  _size = 0 ;

  if (_htableName) {
    Int_t hsize = _htableName->size() ;
    delete _htableName ;
    _htableName = new RooHashTable(hsize) ;   
  }
  if (_htableLink) {
    Int_t hsize = _htableLink->size() ;
    delete _htableLink ;
    _htableLink = new RooHashTable(hsize,RooHashTable::Pointer) ;       
  }
}

//_____________________________________________________________________________
TObject* RooLinkedList::find(const char* name) const 
{
  // Return pointer to object with given name in collection.
  // If no such object is found, return null pointer.

  
  if (_htableName) return _htableName->find(name) ;

  RooLinkedListElem* ptr = _first ;

  if (_useNptr) {
    const TNamed* nptr = RooNameReg::instance().constPtr(name) ;
    
    while(ptr) {
      if ((((RooAbsArg*)ptr->_arg)->namePtr() == nptr)) {
	return ptr->_arg ;
      }
      ptr = ptr->_next ;
    }
    return 0 ;
  }
  
  while(ptr) {
    if (!strcmp(ptr->_arg->GetName(),name)) {      
      return ptr->_arg ;
    }
    ptr = ptr->_next ;
  }
  return 0 ;
}

//_____________________________________________________________________________
RooAbsArg* RooLinkedList::findArg(const RooAbsArg* arg) const 
{
  // Return pointer to object with given name in collection.
  // If no such object is found, return null pointer.
  
  // WVE this will find the wrong entry if the name changed!
  // if (_htableLink) {
  //   return (RooAbsArg*) _htableLink->find(arg) ;
  // }
  
  RooLinkedListElem* ptr = _first ;
  while(ptr) {
    if (((RooAbsArg*)(ptr->_arg))->namePtr() == arg->namePtr()) {
      return (RooAbsArg*) ptr->_arg ;
    }
    ptr = ptr->_next ;
  }
  return 0 ;
}

//_____________________________________________________________________________
Int_t RooLinkedList::IndexOf(const TObject* arg) const 
{
  // Return position of given object in list. If object
  // is not contained in list, return -1

  RooLinkedListElem* ptr = _first;
  Int_t idx(0) ;
  while(ptr) {
    if (ptr->_arg==arg) return idx ;
    ptr = ptr->_next ;
    idx++ ;
  }
  return -1 ;
}

//_____________________________________________________________________________
Int_t RooLinkedList::IndexOf(const char* name) const 
{
  // Return position of given object in list. If object
  // is not contained in list, return -1

  RooLinkedListElem* ptr = _first;
  Int_t idx(0) ;
  while(ptr) {
    if (strcmp(ptr->_arg->GetName(),name)==0) return idx ;
    ptr = ptr->_next ;
    idx++ ;
  }
  return -1 ;
}

//_____________________________________________________________________________
void RooLinkedList::Print(const char* opt) const 
{
  // Print contents of list, defers to Print() function
  // of contained objects
  RooLinkedListElem* elem = _first ;
  while(elem) {
    cout << elem->_arg << " : " ; 
    elem->_arg->Print(opt) ;
    elem = elem->_next ;
  }    
}

//_____________________________________________________________________________
RooLinkedListIter RooLinkedList::iterator(Bool_t dir) const 
{
  return RooLinkedListIter(this,dir) ;
}

//_____________________________________________________________________________
RooFIter RooLinkedList::fwdIterator() const 
{ 
  return RooFIter(this) ; 
}

//_____________________________________________________________________________
TIterator* RooLinkedList::MakeIterator(Bool_t dir) const 
{
  // Return an iterator over this list
  return new RooLinkedListIter(this,dir) ;
}

//_____________________________________________________________________________
void RooLinkedList::Sort(Bool_t ascend) 
{
  if (ascend) _first = mergesort_impl<true>(_first, _size, &_last);
  else _first = mergesort_impl<false>(_first, _size, &_last);
}

//_____________________________________________________________________________
template <bool ascending>
RooLinkedListElem* RooLinkedList::mergesort_impl(
    RooLinkedListElem* l1, const unsigned sz, RooLinkedListElem** tail)
{
  // length 0, 1 lists are sorted
  if (!l1 || sz < 2) {
    // if desired, update the tail of the (newly merged sorted) list
    if (tail) *tail = l1;
    return l1;
  }
  if (sz <= 16) {
    // for short lists, we sort in an array
#if !defined(_WIN32) && !defined(R__SOLARIS_CC50)
    RooLinkedListElem *arr[sz];
#else // _WIN32 && Solaris
    // apparently, MSVC is not clever enough to figure out that sz cannot be
    // zero and is at most sixteen, so we allocate a fixed size array on the
    // stack instead
    RooLinkedListElem *arr[16];
#endif // _WIN32
    for (int i = 0; l1; l1 = l1->_next, ++i) arr[i] = l1;
    // straight insertion sort
    {
	int i = 1;
	do {
	    int j = i - 1;
	    RooLinkedListElem *tmp = arr[i];
	    while (0 <= j) {
		const bool inOrder = ascending ?
		    (tmp->_arg->Compare(arr[j]->_arg) <= 0) :
		    (arr[j]->_arg->Compare(tmp->_arg) <= 0);
		if (!inOrder) break;
		arr[j + 1] = arr[j];
		--j;
	    }
	    arr[j + 1] = tmp;
	    ++i;
	} while (int(sz) != i);
    }
    // link elements in array
    arr[0]->_prev = arr[sz - 1]->_next = 0;
    for (int i = 0; i < int(sz - 1); ++i) {
      arr[i]->_next = arr[i + 1];
      arr[i + 1]->_prev = arr[i];
    }
    if (tail) *tail = arr[sz - 1];
    return arr[0];
  }
  // find middle of l1, and let a second list l2 start there
  RooLinkedListElem *l2 = l1;
  for (RooLinkedListElem *end = l2; end->_next; end = end->_next) {
    end = end->_next;
    l2 = l2->_next;
    if (!end->_next) break;
  }
  // disconnect the two sublists
  l2->_prev->_next = 0;
  l2->_prev = 0;
  // sort the two sublists (only recurse if we have to)
  if (l1->_next) l1 = mergesort_impl<ascending>(l1, sz / 2);
  if (l2->_next) l2 = mergesort_impl<ascending>(l2, sz - sz / 2);
  // merge the two (sorted) sublists
  // l: list head, t: list tail of merged list
  RooLinkedListElem *l = (ascending ? (l1->_arg->Compare(l2->_arg) <= 0) :
	  (l2->_arg->Compare(l1->_arg) <= 0)) ? l1 : l2;
  RooLinkedListElem *t = l;
  if (l == l2) {
    RooLinkedListElem* tmp = l1;
    l1 = l2;
    l2 = tmp;
  }
  l1 = l1->_next;
  while (l1 && l2) {
    const bool inOrder = ascending ? (l1->_arg->Compare(l2->_arg) <= 0) :
	(l2->_arg->Compare(l1->_arg) <= 0);
    if (!inOrder) {
      // insert l2 just before l1
      if (l1->_prev) {
	l1->_prev->_next = l2;
	l2->_prev = l1->_prev;
      }
      // swap l2 and l1
      RooLinkedListElem *tmp = l1;
      l1 = l2;
      l2 = tmp;
    }
    // move forward in l1
    t = l1;
    l1 = l1->_next;
  }
  // attach l2 at t
  if (l2) {
    l2->_prev = t;
    if (t) t->_next = l2;
  }
  // if desired, update the tail of the (newly merged sorted) list
  if (tail) {
    for (l1 = t; l1; l1 = l1->_next) t = l1;
    *tail = t;
  }
  // return the head of the sorted list
  return l;
}
// void Roo1DTable::Streamer(TBuffer &R__b)
// {
//    // Stream an object of class Roo1DTable.

//    if (R__b.IsReading()) {
//       R__b.ReadClassBuffer(Roo1DTable::Class(),this);
//    } else {
//       R__b.WriteClassBuffer(Roo1DTable::Class(),this);
//    }
// }

//_____________________________________________________________________________
void RooLinkedList::Streamer(TBuffer &R__b)
{
  // Custom streaming handling schema evolution w.r.t past implementations

  if (R__b.IsReading()) {

    Version_t v = R__b.ReadVersion();
    //R__b.ReadVersion();
    TObject::Streamer(R__b);

    Int_t size ;
    TObject* arg ;

    R__b >> size ;
    while(size--) {
      R__b >> arg ;
      Add(arg) ;      
    }

    if (v>1 ) {
      R__b >> _name ;
    }

  } else {
    R__b.WriteVersion(RooLinkedList::IsA());
    TObject::Streamer(R__b);
    R__b << _size ;

    RooLinkedListElem* ptr = _first;
    while(ptr) {
      R__b << ptr->_arg ;
      ptr = ptr->_next ;
    }
    R__b << _name ;
  }
}

