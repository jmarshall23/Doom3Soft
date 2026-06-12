/*
===========================================================================

Doom 3 GPL Source Code
Copyright (C) 1999-2011 id Software LLC, a ZeniMax Media company. 

This file is part of the Doom 3 GPL Source Code (?Doom 3 Source Code?).  

Doom 3 Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

#ifndef __LIST_H__
#define __LIST_H__

#include <new>
#include <vector>

/*
===============================================================================

	List template
	Compatibility wrapper over std::vector with the original idList API.

===============================================================================
*/

/*
================
idListSortCompare<type>
================
*/
#ifdef __INTEL_COMPILER
// the intel compiler doesn't do the right thing here
template< class type >
ID_INLINE int idListSortCompare( const type *a, const type *b ) {
	assert( 0 );
	return 0;
}
#else
template< class type >
ID_INLINE int idListSortCompare( const type *a, const type *b ) {
	return *a - *b;
}
#endif

/*
================
idListNewElement<type>
================
*/
template< class type >
ID_INLINE type *idListNewElement( void ) {
	return new type;
}

/*
================
idSwap<type>
================
*/
template< class type >
ID_INLINE void idSwap( type &a, type &b ) {
	type c = a;
	a = b;
	b = c;
}

template< class type >
struct idListStorage {
	typedef type storage_t;

	ID_INLINE static storage_t Store( const type &value ) {
		return value;
	}

	ID_INLINE static type &Ref( storage_t &value ) {
		return value;
	}

	ID_INLINE static const type &ConstRef( const storage_t &value ) {
		return value;
	}

	ID_INLINE static type *Ptr( storage_t *value ) {
		return value;
	}

	ID_INLINE static const type *Ptr( const storage_t *value ) {
		return value;
	}
};

template<>
struct idListStorage<bool> {
	struct storage_t {
		bool value;

		ID_INLINE storage_t( void ) : value( false ) {
		}

		ID_INLINE storage_t( const bool &v ) : value( v ) {
		}

		ID_INLINE storage_t &operator=( const bool &v ) {
			value = v;
			return *this;
		}
	};

	ID_INLINE static storage_t Store( const bool &value ) {
		return storage_t( value );
	}

	ID_INLINE static bool &Ref( storage_t &value ) {
		return value.value;
	}

	ID_INLINE static const bool &ConstRef( const storage_t &value ) {
		return value.value;
	}

	ID_INLINE static bool *Ptr( storage_t *value ) {
		return &value->value;
	}

	ID_INLINE static const bool *Ptr( const storage_t *value ) {
		return &value->value;
	}
};

template< class type >
class idList {
public:

	typedef int		cmp_t( const type *, const type * );
	typedef type	new_t( void );

					idList( int newgranularity = 16 );
					idList( const idList<type> &other );
					~idList<type>( void );

	void			Clear( void );										// clear the list
	int				Num( void ) const;									// returns number of elements in list
	int				NumAllocated( void ) const;							// returns number of elements allocated for
	void			SetGranularity( int newgranularity );				// set new granularity
	int				GetGranularity( void ) const;						// get the current granularity

	size_t			Allocated( void ) const;							// returns total size of allocated memory
	size_t			Size( void ) const;									// returns total size of allocated memory including size of list type
	size_t			MemoryUsed( void ) const;							// returns size of the used elements in the list

	idList<type> &	operator=( const idList<type> &other );
	const type &	operator[]( int index ) const;
	type &			operator[]( int index );

	void			Condense( void );									// resizes list to exactly the number of elements it contains
	void			Resize( int newsize );								// resizes list allocation to the given number of elements
	void			Resize( int newsize, int newgranularity	 );			// resizes list allocation and sets new granularity
	void			SetNum( int newnum, bool resize = true );			// set number of elements in list
	void			AssureSize( int newSize);							// assure list has given number of elements
	void			AssureSize( int newSize, const type &initValue );	// assure list has given number of elements and initialize any new elements
	void			AssureSizeAlloc( int newSize, new_t *allocator );	// assure the pointer list has the given number of elements and allocate any new elements

	type *			Ptr( void );										// returns a pointer to the list
	const type *	Ptr( void ) const;									// returns a pointer to the list
	type &			Alloc( void );										// returns reference to a new data element at the end of the list
	int				Append( const type & obj );							// append element
	int				Append( const idList<type> &other );				// append list
	int				AddUnique( const type & obj );						// add unique element
	int				Insert( const type & obj, int index = 0 );			// insert the element at the given index
	int				FindIndex( const type & obj ) const;				// find the index for the given element
	type *			Find( type const & obj ) const;						// find pointer to the given element
	int				FindNull( void ) const;								// find the index for the first NULL pointer in the list
	int				IndexOf( const type *obj ) const;					// returns the index for the pointer to an element in the list
	bool			RemoveIndex( int index );							// remove the element at the given index
	bool			Remove( const type & obj );							// remove the element
	void			Sort( cmp_t *compare = ( cmp_t * )&idListSortCompare<type> );
	void			SortSubSection( int startIndex, int endIndex, cmp_t *compare = ( cmp_t * )&idListSortCompare<type> );
	void			Swap( idList<type> &other );						// swap the contents of the lists
	void			DeleteContents( bool clear );						// delete the contents of the list

private:
	typedef typename idListStorage<type>::storage_t storage_t;
	typedef std::vector<storage_t> vector_t;
	union vector_storage_t {
		char bytes[ sizeof( vector_t ) ];
		void *alignPtr;
		double alignDouble;
		long double alignLongDouble;
	};

	int				RoundUpToGranularity( int value ) const;
	vector_t &		EnsureList( void );
	vector_t *		List( void );
	const vector_t *List( void ) const;
	void			DestroyList( void );
	void			ReserveExact( int newCapacity );

	int				num;
	int				granularity;
	bool			listConstructed;
	vector_storage_t listStorage;
};

/*
================
idList<type>::idList( int )
================
*/
template< class type >
ID_INLINE idList<type>::idList( int newgranularity ) {
	assert( newgranularity > 0 );
	num = 0;
	granularity = newgranularity;
	listConstructed = false;
}

/*
================
idList<type>::idList( const idList<type> &other )
================
*/
template< class type >
ID_INLINE idList<type>::idList( const idList<type> &other ) {
	num = other.num;
	granularity = other.granularity;
	listConstructed = false;
	if ( other.List() ) {
		new ( &listStorage ) vector_t( *other.List() );
		listConstructed = true;
	}
}

/*
================
idList<type>::~idList<type>
================
*/
template< class type >
ID_INLINE idList<type>::~idList( void ) {
	DestroyList();
	num = 0;
}

/*
================
idList<type>::RoundUpToGranularity
================
*/
template< class type >
ID_INLINE int idList<type>::RoundUpToGranularity( int value ) const {
	int g = granularity;
	if ( g <= 0 ) {
		g = 16;
	}
	value += g - 1;
	value -= value % g;
	return value;
}

/*
================
idList<type>::EnsureList
================
*/
template< class type >
ID_INLINE typename idList<type>::vector_t &idList<type>::EnsureList( void ) {
	if ( !listConstructed ) {
		new ( &listStorage ) vector_t;
		listConstructed = true;
	}
	return *reinterpret_cast<vector_t *>( &listStorage );
}

/*
================
idList<type>::List
================
*/
template< class type >
ID_INLINE typename idList<type>::vector_t *idList<type>::List( void ) {
	return listConstructed ? reinterpret_cast<vector_t *>( &listStorage ) : NULL;
}

/*
================
idList<type>::List
================
*/
template< class type >
ID_INLINE const typename idList<type>::vector_t *idList<type>::List( void ) const {
	return listConstructed ? reinterpret_cast<const vector_t *>( &listStorage ) : NULL;
}

/*
================
idList<type>::DestroyList
================
*/
template< class type >
ID_INLINE void idList<type>::DestroyList( void ) {
	if ( listConstructed ) {
		reinterpret_cast<vector_t *>( &listStorage )->~vector_t();
		listConstructed = false;
	}
}

/*
================
idList<type>::ReserveExact
================
*/
template< class type >
ID_INLINE void idList<type>::ReserveExact( int newCapacity ) {
	assert( newCapacity >= 0 );

	if ( newCapacity <= 0 ) {
		Clear();
		return;
	}

	vector_t &vec = EnsureList();
	if ( static_cast<int>( vec.size() ) == newCapacity ) {
		if ( num > newCapacity ) {
			num = newCapacity;
		}
		return;
	}
	if ( num > newCapacity ) {
		num = newCapacity;
	}

	vector_t temp;
	temp.resize( newCapacity );
	const int copyCount = num < newCapacity ? num : newCapacity;
	for ( int i = 0; i < copyCount; i++ ) {
		temp[ i ] = vec[ i ];
	}
	temp.swap( vec );
}

/*
================
idList<type>::Clear

Frees up the memory allocated by the list.  Assumes that type automatically handles freeing up memory.
================
*/
template< class type >
ID_INLINE void idList<type>::Clear( void ) {
	DestroyList();
	num = 0;
}

/*
================
idList<type>::DeleteContents

Calls the destructor of all elements in the list.  Conditionally frees up memory used by the list.
Note that this only works on lists containing pointers to objects and will cause a compiler error
if called with non-pointers.  Since the list was not responsible for allocating the object, it has
no information on whether the object still exists or not, so care must be taken to ensure that
the pointers are still valid when this function is called.  Function will set all pointers in the
list to NULL.
================
*/
template< class type >
ID_INLINE void idList<type>::DeleteContents( bool clear ) {
	vector_t *vec = List();
	if ( !vec ) {
		return;
	}
	for( int i = 0; i < num; i++ ) {
		delete idListStorage<type>::Ref( ( *vec )[ i ] );
		idListStorage<type>::Ref( ( *vec )[ i ] ) = NULL;
	}

	if ( clear ) {
		Clear();
	} else {
		for ( int i = 0; i < NumAllocated(); i++ ) {
			idListStorage<type>::Ref( ( *vec )[ i ] ) = NULL;
		}
	}
}

/*
================
idList<type>::Allocated

return total memory allocated for the list in bytes, but doesn't take into account additional memory allocated by type
================
*/
template< class type >
ID_INLINE size_t idList<type>::Allocated( void ) const {
	const vector_t *vec = List();
	return vec ? vec->size() * sizeof( type ) : 0;
}

/*
================
idList<type>::Size

return total size of list in bytes, but doesn't take into account additional memory allocated by type
================
*/
template< class type >
ID_INLINE size_t idList<type>::Size( void ) const {
	return sizeof( idList<type> ) + Allocated();
}

/*
================
idList<type>::MemoryUsed
================
*/
template< class type >
ID_INLINE size_t idList<type>::MemoryUsed( void ) const {
	return num * sizeof( type );
}

/*
================
idList<type>::Num

Returns the number of elements currently contained in the list.
Note that this is NOT an indication of the memory allocated.
================
*/
template< class type >
ID_INLINE int idList<type>::Num( void ) const {
	return num;
}

/*
================
idList<type>::NumAllocated

Returns the number of elements currently allocated for.
================
*/
template< class type >
ID_INLINE int idList<type>::NumAllocated( void ) const {
	const vector_t *vec = List();
	return vec ? static_cast<int>( vec->size() ) : 0;
}

/*
================
idList<type>::SetNum

Resize to the exact size specified irregardless of granularity
================
*/
template< class type >
ID_INLINE void idList<type>::SetNum( int newnum, bool resize ) {
	assert( newnum >= 0 );

	if ( resize ) {
		ReserveExact( newnum );
		num = newnum;
	} else {
		if ( newnum > NumAllocated() ) {
			ReserveExact( RoundUpToGranularity( newnum ) );
		}
		num = newnum;
	}
}

/*
================
idList<type>::SetGranularity

Sets the base size of the array and resizes the array to match.
================
*/
template< class type >
ID_INLINE void idList<type>::SetGranularity( int newgranularity ) {
	assert( newgranularity > 0 );
	granularity = newgranularity;

	if ( NumAllocated() > 0 ) {
		ReserveExact( RoundUpToGranularity( Num() ) );
	}
}

/*
================
idList<type>::GetGranularity

Get the current granularity.
================
*/
template< class type >
ID_INLINE int idList<type>::GetGranularity( void ) const {
	return granularity;
}

/*
================
idList<type>::Condense

Resizes the array to exactly the number of elements it contains or frees up memory if empty.
================
*/
template< class type >
ID_INLINE void idList<type>::Condense( void ) {
	if ( Num() > 0 ) {
		ReserveExact( Num() );
	} else {
		Clear();
	}
}

/*
================
idList<type>::Resize

Allocates memory for the amount of elements requested while keeping the contents intact.
================
*/
template< class type >
ID_INLINE void idList<type>::Resize( int newsize ) {
	assert( newsize >= 0 );
	ReserveExact( newsize );
}

/*
================
idList<type>::Resize

Allocates memory for the amount of elements requested while keeping the contents intact.
================
*/
template< class type >
ID_INLINE void idList<type>::Resize( int newsize, int newgranularity ) {
	assert( newsize >= 0 );
	assert( newgranularity > 0 );
	granularity = newgranularity;
	ReserveExact( newsize );
}

/*
================
idList<type>::AssureSize

Makes sure the list has at least the given number of elements.
================
*/
template< class type >
ID_INLINE void idList<type>::AssureSize( int newSize ) {
	assert( newSize >= 0 );
	if ( newSize <= 0 ) {
		num = 0;
		return;
	}
	if ( newSize > NumAllocated() ) {
		ReserveExact( RoundUpToGranularity( newSize ) );
	}
	num = newSize;
}

/*
================
idList<type>::AssureSize

Makes sure the list has at least the given number of elements and initialize any elements not yet initialized.
================
*/
template< class type >
ID_INLINE void idList<type>::AssureSize( int newSize, const type &initValue ) {
	assert( newSize >= 0 );
	if ( newSize <= 0 ) {
		num = 0;
		return;
	}
	if ( newSize > NumAllocated() ) {
		const int oldAllocated = NumAllocated();
		ReserveExact( RoundUpToGranularity( newSize ) );
		vector_t &vec = EnsureList();
		for ( int i = oldAllocated; i < NumAllocated(); i++ ) {
			idListStorage<type>::Ref( vec[ i ] ) = initValue;
		}
	}
	num = newSize;
}

/*
================
idList<type>::AssureSizeAlloc

Makes sure the list has at least the given number of elements and allocates any elements using the allocator.

NOTE: This function can only be called on lists containing pointers. Calling it
on non-pointer lists will cause a compiler error.
================
*/
template< class type >
ID_INLINE void idList<type>::AssureSizeAlloc( int newSize, new_t *allocator ) {
	assert( newSize >= 0 );
	if ( newSize <= 0 ) {
		num = 0;
		return;
	}
	if ( newSize > NumAllocated() ) {
		const int oldAllocated = NumAllocated();
		ReserveExact( RoundUpToGranularity( newSize ) );
		vector_t &vec = EnsureList();
		for ( int i = oldAllocated; i < NumAllocated(); i++ ) {
			idListStorage<type>::Ref( vec[ i ] ) = ( *allocator )();
		}
	}
	num = newSize;
}

/*
================
idList<type>::operator=

Copies the contents and size attributes of another list.
================
*/
template< class type >
ID_INLINE idList<type> &idList<type>::operator=( const idList<type> &other ) {
	if ( this != &other ) {
		num = other.num;
		granularity = other.granularity;
		if ( other.List() ) {
			EnsureList() = *other.List();
		} else {
			DestroyList();
		}
	}
	return *this;
}

/*
================
idList<type>::operator[] const

Access operator.  Index must be within range or an assert will be issued in debug builds.
Release builds do no range checking.
================
*/
template< class type >
ID_INLINE const type &idList<type>::operator[]( int index ) const {
	assert( index >= 0 );
	assert( index < Num() );

	return idListStorage<type>::ConstRef( ( *List() )[ index ] );
}

/*
================
idList<type>::operator[]

Access operator.  Index must be within range or an assert will be issued in debug builds.
Release builds do no range checking.
================
*/
template< class type >
ID_INLINE type &idList<type>::operator[]( int index ) {
	assert( index >= 0 );
	assert( index < Num() );

	return idListStorage<type>::Ref( ( *List() )[ index ] );
}

/*
================
idList<type>::Ptr

Returns a pointer to the begining of the array.  Useful for iterating through the list in loops.

Note: may return NULL if the list is empty.

FIXME: Create an iterator template for this kind of thing.
================
*/
template< class type >
ID_INLINE type *idList<type>::Ptr( void ) {
	vector_t *vec = List();
	return ( !vec || vec->empty() ) ? NULL : idListStorage<type>::Ptr( &( *vec )[0] );
}

/*
================
idList<type>::Ptr

Returns a pointer to the begining of the array.  Useful for iterating through the list in loops.

Note: may return NULL if the list is empty.

FIXME: Create an iterator template for this kind of thing.
================
*/
template< class type >
const ID_INLINE type *idList<type>::Ptr( void ) const {
	const vector_t *vec = List();
	return ( !vec || vec->empty() ) ? NULL : idListStorage<type>::Ptr( &( *vec )[0] );
}

/*
================
idList<type>::Alloc

Returns a reference to a new data element at the end of the list.
================
*/
template< class type >
ID_INLINE type &idList<type>::Alloc( void ) {
	if ( NumAllocated() <= 0 ) {
		ReserveExact( granularity > 0 ? granularity : 16 );
	}
	if ( num == NumAllocated() ) {
		const int newSize = NumAllocated() > 0 ? NumAllocated() * 2 : ( granularity > 0 ? granularity : 16 );
		ReserveExact( newSize );
	}
	return idListStorage<type>::Ref( EnsureList()[ num++ ] );
}

/*
================
idList<type>::Append

Increases the size of the list by one element and copies the supplied data into it.

Returns the index of the new element.
================
*/
template< class type >
ID_INLINE int idList<type>::Append( type const & obj ) {
	if ( NumAllocated() <= 0 ) {
		ReserveExact( granularity > 0 ? granularity : 16 );
	}
	if ( num == NumAllocated() ) {
		const int newSize = NumAllocated() > 0 ? NumAllocated() * 2 : ( granularity > 0 ? granularity : 16 );
		ReserveExact( newSize );
	}
	idListStorage<type>::Ref( EnsureList()[ num ] ) = obj;
	num++;
	return num - 1;
}


/*
================
idList<type>::Insert

Increases the size of the list by at leat one element if necessary 
and inserts the supplied data into it.

Returns the index of the new element.
================
*/
template< class type >
ID_INLINE int idList<type>::Insert( type const & obj, int index ) {
	if ( index < 0 ) {
		index = 0;
	} else if ( index > Num() ) {
		index = Num();
	}
	if ( NumAllocated() <= 0 && granularity > 0 ) {
		ReserveExact( granularity );
	}
	if ( num == NumAllocated() ) {
		const int newSize = NumAllocated() > 0 ? NumAllocated() * 2 : ( granularity > 0 ? granularity : 16 );
		ReserveExact( newSize );
	}
	vector_t &vec = EnsureList();
	for ( int i = num; i > index; i-- ) {
		vec[ i ] = vec[ i - 1 ];
	}
	num++;
	idListStorage<type>::Ref( vec[ index ] ) = obj;
	return index;
}

/*
================
idList<type>::Append

adds the other list to this one

Returns the size of the new combined list
================
*/
template< class type >
ID_INLINE int idList<type>::Append( const idList<type> &other ) {
	const int n = other.Num();
	if ( n <= 0 ) {
		return Num();
	}
	if ( NumAllocated() < Num() + n ) {
		ReserveExact( RoundUpToGranularity( Num() + n ) );
	}
	for ( int i = 0; i < n; i++ ) {
		Append( other[i] );
	}
	return Num();
}

/*
================
idList<type>::AddUnique

Adds the data to the list if it doesn't already exist.  Returns the index of the data in the list.
================
*/
template< class type >
ID_INLINE int idList<type>::AddUnique( type const & obj ) {
	int index;

	index = FindIndex( obj );
	if ( index < 0 ) {
		index = Append( obj );
	}

	return index;
}

/*
================
idList<type>::FindIndex

Searches for the specified data in the list and returns it's index.  Returns -1 if the data is not found.
================
*/
template< class type >
ID_INLINE int idList<type>::FindIndex( type const & obj ) const {
	const vector_t *vec = List();
	if ( !vec ) {
		return -1;
	}
	for( int i = 0; i < Num(); i++ ) {
		if ( idListStorage<type>::ConstRef( ( *vec )[ i ] ) == obj ) {
			return i;
		}
	}

	// Not found
	return -1;
}

/*
================
idList<type>::Find

Searches for the specified data in the list and returns it's address. Returns NULL if the data is not found.
================
*/
template< class type >
ID_INLINE type *idList<type>::Find( type const & obj ) const {
	int i;

	i = FindIndex( obj );
	if ( i >= 0 ) {
		return const_cast<type *>( &idListStorage<type>::ConstRef( ( *List() )[ i ] ) );
	}

	return NULL;
}

/*
================
idList<type>::FindNull

Searches for a NULL pointer in the list.  Returns -1 if NULL is not found.

NOTE: This function can only be called on lists containing pointers. Calling it
on non-pointer lists will cause a compiler error.
================
*/
template< class type >
ID_INLINE int idList<type>::FindNull( void ) const {
	const vector_t *vec = List();
	if ( !vec ) {
		return -1;
	}
	for( int i = 0; i < Num(); i++ ) {
		if ( idListStorage<type>::ConstRef( ( *vec )[ i ] ) == NULL ) {
			return i;
		}
	}

	// Not found
	return -1;
}

/*
================
idList<type>::IndexOf

Takes a pointer to an element in the list and returns the index of the element.
This is NOT a guarantee that the object is really in the list. 
Function will assert in debug builds if pointer is outside the bounds of the list,
but remains silent in release builds.
================
*/
template< class type >
ID_INLINE int idList<type>::IndexOf( type const *objptr ) const {
	const type *base = Ptr();
	int index = base ? static_cast<int>( objptr - base ) : -1;

	assert( index >= 0 );
	assert( index < Num() );

	return index;
}

/*
================
idList<type>::RemoveIndex

Removes the element at the specified index and moves all data following the element down to fill in the gap.
The number of elements in the list is reduced by one.  Returns false if the index is outside the bounds of the list.
================
*/
template< class type >
ID_INLINE bool idList<type>::RemoveIndex( int index ) {
	assert( index >= 0 );
	assert( index < Num() );

	if ( ( index < 0 ) || ( index >= Num() ) ) {
		return false;
	}

	num--;
	vector_t &vec = EnsureList();
	for ( int i = index; i < num; i++ ) {
		vec[ i ] = vec[ i + 1 ];
	}
	return true;
}

/*
================
idList<type>::Remove

Removes the element if it is found within the list and moves all data following the element down to fill in the gap.
The number of elements in the list is reduced by one.  Returns false if the data is not found in the list.
================
*/
template< class type >
ID_INLINE bool idList<type>::Remove( type const & obj ) {
	int index;

	index = FindIndex( obj );
	if ( index >= 0 ) {
		return RemoveIndex( index );
	}
	
	return false;
}

/*
================
idList<type>::Sort

Performs a qsort on the list using the supplied comparison function.  Note that the data is merely moved around the
list, so any pointers to data within the list may no longer be valid.
================
*/
template< class type >
ID_INLINE void idList<type>::Sort( cmp_t *compare ) {
	const vector_t *vec = List();
	if ( !vec || vec->empty() ) {
		return;
	}
	typedef int cmp_c(const void *, const void *);

	cmp_c *vCompare = (cmp_c *)compare;
	qsort( ( void * )Ptr(), ( size_t )Num(), sizeof( type ), vCompare );
}

/*
================
idList<type>::SortSubSection

Sorts a subsection of the list.
================
*/
template< class type >
ID_INLINE void idList<type>::SortSubSection( int startIndex, int endIndex, cmp_t *compare ) {
	const vector_t *vec = List();
	if ( !vec || vec->empty() ) {
		return;
	}
	if ( startIndex < 0 ) {
		startIndex = 0;
	}
	if ( endIndex >= Num() ) {
		endIndex = Num() - 1;
	}
	if ( startIndex >= endIndex ) {
		return;
	}
	typedef int cmp_c(const void *, const void *);

	cmp_c *vCompare = (cmp_c *)compare;
	qsort( ( void * )( &Ptr()[startIndex] ), ( size_t )( endIndex - startIndex + 1 ), sizeof( type ), vCompare );
}

/*
================
idList<type>::Swap

Swaps the contents of two lists
================
*/
template< class type >
ID_INLINE void idList<type>::Swap( idList<type> &other ) {
	idSwap( num, other.num );
	idSwap( granularity, other.granularity );

	if ( listConstructed && other.listConstructed ) {
		EnsureList().swap( other.EnsureList() );
	} else if ( listConstructed ) {
		new ( &other.listStorage ) vector_t;
		other.listConstructed = true;
		EnsureList().swap( other.EnsureList() );
		DestroyList();
	} else if ( other.listConstructed ) {
		new ( &listStorage ) vector_t;
		listConstructed = true;
		EnsureList().swap( other.EnsureList() );
		other.DestroyList();
	}
}

#endif /* !__LIST_H__ */
