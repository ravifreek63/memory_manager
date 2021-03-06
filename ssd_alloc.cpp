#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include "errno.h"
#include <sys/mman.h>
#include <iostream>
#include <unordered_map>
#include <unistd.h>
#include <signal.h>
#include "ssd_alloc.h"
#include "assert.h"
#include "string.h"

using namespace std;

#define handle_error(msg) \
  do {perror (msg); exit(EXIT_FAILURE);} while (0)

#define PAGE_BUFFER_SIZE 250*25 // size ~ 10 MB // The page buffer has PAGE_BUFFER_SIZE pages in memory
int PAGE_SIZE =  sysconf(_SC_PAGE_SIZE); // Each page in page buffer is of size 4KB

struct page_buffer_str {
	void *page_buffer_ptrs[PAGE_BUFFER_SIZE]; // This is the array which stores the location of all the pages in the page buffer,
	 // location in memory where all the pages are materialized
	bool page_dirty[PAGE_BUFFER_SIZE]; // This keeps a track of whether each of the page in page buffer is dirty or not
	bool page_buffer_bitmap[PAGE_BUFFER_SIZE]; // Have to convert it to an actual bitmap
	int pagesInBuffer; // Keeps a count of the number of pages free in the page buffer
};

struct page_buffer_str page_buffer;
struct sigaction sa;

unordered_map <void*, struct object> object_table; // This is the object table which stores the exact location of objects to fetch them
std::unordered_map<void *, struct object>::iterator object_table_it; // Iterator of the map
std::queue <void *> fifo_list; // This is a list, which maintains all the objects in the page buffer in FIFO order

// Function which converts a page header address to object's location
void *page_header_to_object_va (void *ph){
	return (void *)((struct page_header *)ph + 1);
}

// Function which converts an object's location to the location where its page header resides
void *object_va_to_page_header(void *object_va){
  return (void *)((long)object_va & (~(PAGE_SIZE-1)));
}

// This function updates the object table with a given object header
void insert_object (struct page_header *ph, void *page_address){
  // Build the object from the page header 
  struct object ob = get_object_from_header (ph, page_address);
  // Insert the object into the object table 
  object_table.insert (pair <void *, struct object> (page_address, ob));
  // Inserting the object in the fifo list of the page buffer 
  //  fifo_list.push (page_address);
}

// This function gets an object from a page header
struct object get_object_from_header (struct page_header *ph, void *page_address){
  struct object ob;
  void *start_object = page_header_to_object_va (page_address);
  ob.size = ph->object_size;
  ob.value = (void *) malloc (sizeof (ph->object_size));
  memcpy (ob.value, start_object, ph->object_size);
  return ob;
}

// The handler to catch SIGSEGV faults on memory access 
void seg_handler(int sig, siginfo_t *si, void *unused){
  // printf("Got SIGSEGV at address: 0x%lx\n", (long) si->si_addr);
  // printf("Signal Code %d\n", si->si_code);
  //  fflush (stdout);
  // Calling the materialize page function 
  if (si->si_code == SEGV_ACCERR){
    void *page_header = object_va_to_page_header(si->si_addr);
    mprotect (page_header, PAGE_SIZE, PROT_READ | PROT_WRITE); // after page materialization the protection levels of the page are changed    
    materialize_page (page_header);   // materializing the page from the object table    
    // pushing the page into the fifo list 
    fifo_list.push(page_header);
    //  printf ("materializing page 0x%lx\n", page_header);
    // fflush (stdout);
  }
  else
    handle_error ("Segmentation fault, Code is different");
}

void init_ssd_alloc (void){
// Initializing the page buffer to get memory space from for storing the pages in the page buffer
  page_buffer.pagesInBuffer = PAGE_BUFFER_SIZE; // Initially the number of pages in the page buffer is equal to the total size of the page buffer
// defining the segmentation fault handler
  sa.sa_flags = SA_SIGINFO; // The siginfo_t structure is passed as a second parameter to the user signal handler function
  sigemptyset(&sa.sa_mask); // Emptying the signal set associated with the structure sigaction_t 
  sa.sa_sigaction = seg_handler; // Assigning the fault handler 
  if (sigaction(SIGSEGV, &sa, NULL) == -1) // Installs the function in sa taken on a segmentation fault
    handle_error("sigaction");
}

// Constructs a new object of the given size
struct object init_object(int size){
  struct object ob;
  ob.value = malloc (size); // the object is initialized
  ob.size = size; 			// storing the size of the object
  return ob;
}

// Allocates an object and currently stores it in object table
void *ssd_oalloc (int num_objects, int size_object){
  assert (num_objects == 1);
  // Insert the object in object table
  // Allocating page in memory to the object, currently we implement it for objects of size < 4KB and num_objects = 1
  void *header_location;
  posix_memalign (&(header_location), PAGE_SIZE, PAGE_SIZE); // object_location is the virtual memory address of the object
  
  object_table.insert(pair <void *, struct object> (header_location, init_object (size_object))); // On an initialization, some memory is allocated to the object and is pushed into memory
  // Protecting the page so that on each access the page faults, protection mechanism is for any access 
  if (mprotect ((void *) header_location, PAGE_SIZE, PROT_NONE) == -1){
    handle_error("mprotect Error");
  }
  // Initially the page is advised to be not needed
  madvise (header_location, PAGE_SIZE, MADV_DONTNEED);
  // Each of the page has a header which denotes the size of the object on the page
  void *object_location = page_header_to_object_va (header_location);
  if (object_location == NULL){
    printf("ssd alloc returns null\n");
    fflush (stdout);
    exit (-1);

  }
  return (object_location);
}

// The current eviction policy is based on random selection and eviction of a page from the page buffer.
int select_random_page (void){
  /* initialize random seed: */
  srand (time(NULL));
  /* returns a random page from the page buffer: */
  return (rand() % PAGE_BUFFER_SIZE);
}

// Function which gets a free page from the page buffer 
int get_free_page (void){
  int count, victim_page_index = -1;
  void *page_address;
  for (count = 0; count < PAGE_BUFFER_SIZE; count++){
    if (!(page_buffer.page_buffer_bitmap[count])){
    	victim_page_index = count;
    	break;
    }
  }

  // Checking if a free page has been found within the page buffer 
  // 	assert (count == PAGE_BUFFER_SIZE);
  	if (victim_page_index == -1){
	  //		printf ("page buffer is filled, evicting a page");
		// Page eviction is not done as yet, hence evicting the current page
	  //		victim_page_index = select_random_page();
	  //		void *victim_page_address = page_buffer.page_buffer_ptrs[victim_page_index] ;
		//	printf ("victim page address = 0x%lx\n", (long) (victim_page_address));
	  void *victim_page_address = fifo_list.front ();
	  fifo_list.pop();
		
	  evict_page (victim_page_address);
  	}
    remove_page_buffer (victim_page_index);
    // Denoting that a page in page buffer is now empty
    return victim_page_index;
}

void remove_page_buffer (int index){
    // Page Removal --> Bitmap Is Set To Not Occupied
	page_buffer.page_buffer_bitmap [index] = false;
    // Reducing the number of pages in page buffer
	page_buffer.pagesInBuffer--;
	// The number of pages in page buffer > 0
    assert (page_buffer.pagesInBuffer >= 0);
}

/* This function evicts a victim page from the page buffer: */
void evict_page (void *victim_page_address){
  // Store the page back to the page table
  // We would need the size of the object on the page also 
  struct page_header *ph = (struct page_header *) malloc (sizeof (struct page_header));
  // Copying the header from the page to get object's size
  memcpy (ph, victim_page_address, sizeof(page_header));
  int object_size = ph->object_size;
  // Saving the object back to the object table
  insert_object (ph, victim_page_address);
  // protect the page 
  mprotect (victim_page_address, PAGE_SIZE, PROT_NONE);
  // madvise command called to free this page
  madvise (victim_page_address, PAGE_SIZE, MADV_DONTNEED);
  free (ph);
}

// The page materialization function gets an argument as the address of the object and returns the address of the page 
void materialize_page (void *va){
   // Getting a free page from the page buffer
  int free_page_index = get_free_page ();
  // Getting the object our from the object table
  object_table_it = object_table.find (va);
  // Checking if no free page is found 
  if (object_table_it == object_table.end()){
    printf ("Object with address 0x%lx not found in the object table \n", (long)va);
    exit (EXIT_FAILURE);
  }
  // Getting the object out
  struct object ob = object_table_it->second; 
  // Constructing the page header 
  struct page_header *ph = (struct page_header *) malloc (sizeof(struct page_header)); // has to be freed later on 
  ph->object_size = ob.size; // assigning the object size to the page header
  //  printf ("Materializing object, Object Size = %d\n", ob.size);
  memcpy(va, ph, sizeof (struct page_header)); // copying the page header to the page in the page buffer
  memcpy(page_header_to_object_va(va), ob.value, ob.size); // copying the object to the materialized page
  // the memory allocated by to store the object in the object table is freed because on materializing an object the page has to be removed from the object table 
  free (ob.value); // this prevents memory leakage 
  free (ph); // the page header is freed here
  insert_page_buffer (free_page_index, va); 
  object_table.erase (object_table_it);
}

void insert_page_buffer (int index, void *page_address){
  // Updating the page buffer ptr to the page
  page_buffer.page_buffer_ptrs[index] = page_address;
  // Indicating that the page is occupied
  page_buffer.page_buffer_bitmap[index] = true;
  // Incrementing the size of the page buffer
  page_buffer.pagesInBuffer++;
} 
