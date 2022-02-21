#pragma once
#ifndef GREEZEZ_MPSCQUEUE_HPP
#define GREEZEZ_MPSCQUEUE_HPP


#include <atomic>


#ifndef GREEZEZ_CACHE_LINE_SIZE
#define GREEZEZ_CACHE_LINE_SIZE 64
#endif



namespace greezez
{

	namespace details
	{

		template<typename T>
		class List
		{

			struct Node
			{

				template<typename ... ARGS>
				Node(ARGS&& ... args) noexcept :
					next(nullptr), item(std::forward<ARGS>(args) ...)
				{}

				Node* next;
				T item;
			};

		public:

			List() noexcept :
				head_(nullptr), current_(nullptr), size_(0)
			{}


			~List()
			{
				clear();
			}


			T& front() noexcept
			{
				return head_->item;
			}


			T& current() noexcept
			{
				return current_->item;
			}


			void updateCurrent() noexcept
			{
				if (current_->next != nullptr)
				{
					current_ = current_->next;
					return;
				}

				resetCurrent();
			}


			void resetCurrent() noexcept
			{
				current_ = head_;
			}


			template<typename ... ARGS>
			bool emplaceFront(ARGS&& ... args) noexcept
			{
				Node* node = new(std::nothrow) Node(std::forward<ARGS>(args) ...);

				if (node == nullptr)
					return false;

				node->next = head_;
				head_ = node;

				if (current_ == nullptr)
					resetCurrent();

				size_++;

				return true;
			}


			template<typename ... ARGS>
			bool emplaceAndUpdateCurrent(ARGS&& ... args) noexcept
			{
				if (head_ == nullptr)
				{
					if (emplaceFront(std::forward<ARGS>(args)...))
						return true;

					return false;
				}

				Node* node = new(std::nothrow) Node(std::forward<ARGS>(args) ...);

				if (node == nullptr)
					return false;;

				node->next = current_->next;
				current_->next = node;
				current_ = node;

				size_++;

				return true;
			}


			void popFront() noexcept
			{
				if (head_ == nullptr)
					return;

				Node* node = head_;
				head_ = node->next;

				size_--;

				delete node;
			}


			void clear() noexcept
			{

				while (size_ != 0)
					popFront();

				size_ = 0;
				current_ = nullptr;
			}


			size_t size() noexcept
			{
				return size_;
			}


			bool empty() noexcept
			{
				return size_ == 0;
			}


			template<typename Fn>
			void forEach(Fn fn)
			{
				Node* node = head_;

				while (node != nullptr)
				{
					fn(node->item);

					node = node->next;
				}
			}

		private:

			Node* head_;
			Node* current_;

			size_t size_;

		};



		class Data
		{

		public:

			struct Header
			{
				std::atomic_size_t numOfAcquired;
				char padding1[GREEZEZ_CACHE_LINE_SIZE - sizeof(std::atomic_size_t)];

				size_t offset;
				size_t chunkCount;
				size_t flag;
				char padding2[GREEZEZ_CACHE_LINE_SIZE - (sizeof(size_t) * 3)];
			};

			static constexpr size_t ChunkSize = GREEZEZ_CACHE_LINE_SIZE;
			static constexpr size_t HeaderSize = sizeof(Header);

			static_assert(HeaderSize == ChunkSize * 2, "sizeof(DataBlockHeader) != 128 byte!!!");


			Data(size_t chunkCount, bool& success) noexcept :
				header_(nullptr), data_(nullptr)
			{
				data_ = std::malloc(HeaderSize + (chunkCount * ChunkSize));

				if (data_ == nullptr)
				{
					success = false;
					return;
				}

				header_ = static_cast<Header*>(data_);

				header_->numOfAcquired = 0;
				header_->offset = 0;
				header_->chunkCount = chunkCount;
				header_->flag = 0;

				success = true;
			}


			~Data()
			{
				std::free(data_);
			}


			void* acquire(size_t chunkCount) noexcept
			{
				if (reset())
					return nullptr;

				if (chunkCount > (header_->chunkCount - header_->offset))
				{
					header_->offset = 1;
					return nullptr;
				}

				void* ptr = static_cast<void*>(static_cast<uint8_t*>(data_) + HeaderSize + header_->offset * ChunkSize);
				header_->offset += chunkCount;

				header_->numOfAcquired.fetch_add(1, std::memory_order_release);

				return ptr;
			}


			size_t offset() noexcept
			{
				return header_->offset;
			}


			size_t chunkCount() noexcept
			{
				return header_->chunkCount;
			}


		private:

			bool reset() noexcept
			{
				if (header_->flag == 0)
					return false;

				if (header_->numOfAcquired.load(std::memory_order_release) == 0)
				{
					header_->offset = 0;
					header_->flag = 0;

					return false;
				}

				return true;
			}

		private:

			Data::Header* header_;
			void* data_;
		};

	}



	namespace mpsc
	{

		class UniqueDataPool;
		class Queue;


		// The UniqueData class provides wrapper for raw data, allocated in pool or heap.
		class UniqueData
		{

			friend class UniqueDataPool;
			friend class Queue;

		public:

			enum class State : uint8_t
			{
				Recorded = 0,
				Utilized,
			};


			enum class AllocType : uint8_t
			{
				Pool = 0,
				Heap,

				// needed for dummy UniqueData
				Stack,
			};


			// Constructs a UniqueData
			//
			// Creates an empty UniqueData class.
			UniqueData() noexcept :
				state_(State::Recorded), allocType_(AllocType::Pool), offset_(0),
				data_(nullptr), next_(nullptr), padding(0)
			{}


			UniqueData(const UniqueData&) = delete;
			UniqueData& operator=(const UniqueData& other) = delete;


			UniqueData(UniqueData&& other) noexcept :
				state_(other.state_), allocType_(other.allocType_), offset_(other.offset_),
				data_(other.data_), next_(nullptr), padding(0)
			{
				other.data_ = nullptr;
			}


			UniqueData& operator=(UniqueData&& other) noexcept
			{
				if (&other == this)
					return *this;

				state_ = other.state_;
				allocType_ = other.allocType_;
				offset_ = other.offset_;
				data_ = other.data_;
				next_ = nullptr;

				other.data_ = nullptr;

				return *this;
			}


			~UniqueData()
			{
				release();
			}


			// Creates UniqueData, size <TypeSize> in heap.
			template<size_t TypeSize>
			static UniqueData* make() noexcept
			{
				void* ptr = std::malloc(sizeof(UniqueData) + TypeSize);

				if (ptr != nullptr)
				{
					UniqueData* uniqueData = new(ptr) UniqueData(UniqueData::State::Recorded, UniqueData::AllocType::Heap, 0, ptr);
					return uniqueData;
				}

				return nullptr;
			}


			// return raw pointer to data.
			//
			// recommend: use with method UniqueData::isValid().
			void* raw() noexcept
			{
				return static_cast<void*>(static_cast<uint8_t*>(data_) + sizeof(UniqueData));
			}


			// return pointer to casted in type <T>
			//
			// recommend: use with method UniqueData::isValid().
			template<typename T>
			T* get() noexcept
			{
				return static_cast<T*>(raw());
			}

			
			// places a class <T>, with arguments <ARGS>, in the data.
			//
			// recommend: use with method UniqueData::isValid().
			template<typename T, typename ... ARGS>
			T* emplace(ARGS&& ... args) noexcept
			{
				return new(raw()) T(std::forward<ARGS>(args) ...);
			}


			// if the date is allocated on the heap, frees the data with the method std::free!
			// if the date is allocated on the pool, returns data to the pool.
			void release() noexcept
			{
				if (!isValid())
					return;

				if (allocType_ == AllocType::Pool)
				{
					details::Data::Header* dataHeader = reinterpret_cast<details::Data::Header*>
						(static_cast<uint8_t*>(data_) - (details::Data::HeaderSize + offset_ * details::Data::ChunkSize));

					dataHeader->numOfAcquired.fetch_sub(1, std::memory_order_release);
					
					return;
				}

				if (allocType_ == AllocType::Heap)
				{
					std::free(data_);
					return;
				}
			}


			// return true, if UniqueData owned data.
			bool isValid() noexcept
			{
				return data_ != nullptr;
			}


		private:

			// constructor for class friends
			UniqueData(State state, AllocType allocType, uint32_t offset, void* data) noexcept :
				state_(state), allocType_(allocType), offset_(offset),
				data_(data), next_(nullptr), padding(0)
			{}


			// constructor for dummu UniqueData
			UniqueData(nullptr_t) noexcept :
				state_(State::Utilized), allocType_(AllocType::Stack), offset_(0),
				data_(nullptr), next_(nullptr), padding(0)
			{}


		private:

			State state_;
			AllocType allocType_;

			uint16_t padding;
			uint32_t offset_;

			void* data_;
			std::atomic<UniqueData*> next_;
		};


		// The UniqueDataPool class provides dynamic pool UniqueData.
		class UniqueDataPool
		{

		public:

			// Constructs a UniqueDataPool
			//
			// dataBlockCount, number of data blocks.
			// 
			// numOfChunkInDataBlock, number of chunk in a data block.
			// chunk size == GREEZEZ_CACHE_LINE_SIZE (default 64 byte).
			// final size of one data block == (128 byte header) + numOfChunkInDataBlock * GREEZEZ_CACHE_LINE_SIZE  
			// 
			// success == true, if all data is allocated correctly.
			// success == false, if there is not enough memory to allocate data.
			UniqueDataPool(size_t dataBlockCount, size_t numOfChunkInDataBlock, bool& success) noexcept :
				numOfChunkInDataBlock_(numOfChunkInDataBlock), dataList_()
			{
				bool allocSuccess = false;

				for (size_t i = 0; i < dataBlockCount; i++)
				{
					if (!dataList_.emplaceFront(numOfChunkInDataBlock, allocSuccess) or !allocSuccess)
					{
						dataList_.clear();
						success = false;
						return;
					}
				}
				
				success = true;
			}


			~UniqueDataPool()
			{
				dataList_.clear();
			}


			// tries to allocate data of size <TypeSize>.
			//
			// return nulptr, if pool full.
			template<size_t TypeSize>
			UniqueData* tryAcquire() noexcept
			{
				constexpr size_t blockCount = (sizeof(UniqueData) + TypeSize <= details::Data::ChunkSize)
					? 1 : ((sizeof(UniqueData) + TypeSize) / details::Data::ChunkSize) + 1;

				bool firstTry = true;

				while (true)
				{
					details::Data& data = dataList_.current();
					uint32_t currentOffset = static_cast<uint32_t>(data.offset());

					void* ptr = data.acquire(blockCount);

					if (ptr != nullptr)
					{
						UniqueData* uniqueData = new(ptr) UniqueData(UniqueData::State::Recorded, UniqueData::AllocType::Pool, currentOffset, ptr);
						return uniqueData;
					}

					if (!firstTry)
						return nullptr;

					firstTry = false;

					dataList_.updateCurrent();
				}
			}


			// allocates data of size <TypeSize>, if the pool is full then allocate a new block of data.
			//
			// retutn nullptr, if there is not enough memory to allocate block of data.
			template<size_t TypeSize>
			UniqueData* acquire() noexcept
			{
				UniqueData* uniqueData = tryAcquire<TypeSize>();

				if (uniqueData != nullptr)
					return uniqueData;

				bool succes = false;
				if (!dataList_.emplaceAndUpdateCurrent(numOfChunkInDataBlock_, succes) or !succes)
					return nullptr;

				return tryAcquire<TypeSize>();
			}

			// return the number of data blocks
			size_t size() noexcept
			{
				return dataList_.size();
			}


		private:

			size_t numOfChunkInDataBlock_;
			details::List<details::Data> dataList_;
		};


		// The Queue class provides a multi-writer/single-reader fifo queue, pushing and popping is wait-free.
		class alignas(GREEZEZ_CACHE_LINE_SIZE) Queue
		{

		public:

			// Constructs a Queue
			
			Queue() noexcept :
				head_(nullptr), dummy(nullptr), tail_(nullptr)
			{
				head_.store(&dummy, std::memory_order_relaxed);
				tail_.store(&dummy, std::memory_order_release);	
			}


			~Queue()
			{
			}


			
			// Pops object from queue.
			//
			// return UniqueData pointer, if queue is not empty. 
			// return nullptr, if queue empty or first element empty.
			//
			// non-blocking and non-thread-safe.
			// 
			// CAREFULL! only one thread is allowed to pop UniqueData to the Queue.
			UniqueData* pop() noexcept
			{				
				while (true)
				{
					UniqueData* head = head_.load(std::memory_order_acquire);

					if (head->state_ == UniqueData::State::Recorded)
					{
						head->state_ = UniqueData::State::Utilized;
						return head;
					}

					UniqueData* nextHead = head->next_.load(std::memory_order_acquire);

					if (nextHead == nullptr)
						return nullptr;

					head_.store(nextHead, std::memory_order_release);

					continue;

				}
			}


			// Pushes object UniqueData to the queue.
			//
			// return false, if UniqueData = nullptr
			// return true, if the push operation is successful.
			//
			// thread-safe and non-blocking.
			//  
			// Any thread can pushed UniqueData in queue.
			bool push(UniqueData* uniqueData) noexcept
			{
				if (uniqueData == nullptr)
					return false;

				while (true)
				{
					UniqueData* tail = tail_.load(std::memory_order_acquire);
					UniqueData* next = tail->next_.load(std::memory_order_acquire);

					if (next == nullptr)
					{
						if (tail->next_.compare_exchange_weak(next, uniqueData))
						{
							tail_.compare_exchange_strong(tail, uniqueData);

							return true;
						}
					}
					else
					{
						tail_.compare_exchange_strong(tail, next);
					}
				}

				return false;
			}


		private:

			std::atomic<UniqueData*> head_;
			UniqueData dummy;
			char padding1[GREEZEZ_CACHE_LINE_SIZE - (sizeof(std::atomic<UniqueData*>) + sizeof(void*))]{};

			std::atomic<UniqueData*> tail_;
			char padding2[GREEZEZ_CACHE_LINE_SIZE - sizeof(std::atomic<UniqueData*>)]{};

		};
	}


}

#endif // !GREEZEZ_MPSCQUEUE_HPP