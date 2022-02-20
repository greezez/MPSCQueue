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
				size_t blockCount;
				size_t flag;
				char padding2[GREEZEZ_CACHE_LINE_SIZE - (sizeof(size_t) * 3)];
			};

			static constexpr size_t BlockSize = GREEZEZ_CACHE_LINE_SIZE;
			static constexpr size_t HeaderSize = sizeof(Header);

			static_assert(HeaderSize == BlockSize * 2, "sizeof(DataBlockHeader) != 128 byte!!!");


			Data(size_t blockCount, bool& success) noexcept :
				header_(nullptr), data_(nullptr)
			{
				data_ = std::malloc(HeaderSize + blockCount * BlockSize);

				if (data_ == nullptr)
				{
					success = false;
					return;
				}

				header_ = static_cast<Header*>(data_);

				header_->numOfAcquired = 0;
				header_->offset = 0;
				header_->blockCount = blockCount;
				header_->flag = 0;

				success = true;
			}


			~Data()
			{
				std::free(data_);
			}


			void* acquire(size_t blockCount) noexcept
			{
				if (reset())
					return nullptr;

				if (blockCount > (header_->blockCount - header_->offset))
				{
					header_->offset = 1;
					return nullptr;
				}

				void* ptr = static_cast<void*>(static_cast<uint8_t*>(data_) + HeaderSize + header_->offset * BlockSize);
				header_->offset += blockCount;

				header_->numOfAcquired.fetch_add(1, std::memory_order_release);

				return ptr;
			}


			size_t offset() noexcept
			{
				return header_->offset;
			}


			size_t blockCount() noexcept
			{
				return header_->blockCount;
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
			};


			// Constructs a UniqueData
			//
			// Ñreates an empty UniqueData class.
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

				if (allocType_ == AllocType::Heap)
				{
					std::free(data_);
					return;
				}

				details::Data::Header* dataHeader = reinterpret_cast<details::Data::Header*>
					(static_cast<uint8_t*>(data_) - (details::Data::HeaderSize + offset_ * details::Data::BlockSize));

				dataHeader->numOfAcquired.fetch_sub(1, std::memory_order_release);
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


			// constructor for class friends
			UniqueData(nullptr_t, void* data) noexcept :
				state_(State::Utilized), allocType_(AllocType::Heap), offset_(0),
				data_(data), next_(nullptr), padding(0)
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
			// poolSize, number of memory blocks.
			// dataBlockSize, the number of blocks that make up the raw data. The block size is GREEZEZ_CACHE_LINE_SIZE == 64 in default.
			//		final size: dataBlockSize * GREEZEZ_CACHE_LINE_SIZE.
			// 
			// success == true, if all data is allocated correctly.
			// success == false, if there is not enough memory to allocate data.
			UniqueDataPool(size_t poolSize, size_t dataBlockSize, bool& success) noexcept :
				dataBlockSize_(dataBlockSize), dataList_()
			{
				bool allocSuccess = false;

				for (size_t i = 0; i < poolSize; i++)
				{
					if (!dataList_.emplaceFront(dataBlockSize, allocSuccess) or !allocSuccess)
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
				constexpr size_t blockCount = (sizeof(UniqueData) + TypeSize <= details::Data::BlockSize)
					? 1 : ((sizeof(UniqueData) + TypeSize) / details::Data::BlockSize) + 1;

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
				if (!dataList_.emplaceAndUpdateCurrent(dataBlockSize_, succes) or !succes)
					return nullptr;

				return tryAcquire<TypeSize>();
			}

			// return the number of data blocks
			size_t size() noexcept
			{
				return dataList_.size();
			}


		private:

			size_t dataBlockSize_;
			details::List<details::Data> dataList_;
		};


		// The Queue class provides a multi-writer/single-reader fifo queue, pushing and popping is wait-free.
		class alignas(GREEZEZ_CACHE_LINE_SIZE) Queue
		{

		public:

			// Constructs a Queue
			//
			// success = false, if a memory allocation error occurs for the first element(dummy UniqueData) of the queue.
			Queue(bool& succes) noexcept :
				head_(nullptr), dummy(nullptr), tail_(nullptr), numOfInQueue_(0)
			{
				dummy = std::malloc(sizeof(UniqueData));

				if (dummy != nullptr)
				{
					UniqueData* uniqueData = new(dummy) UniqueData(nullptr, dummy);

					head_.store(uniqueData, std::memory_order_relaxed);
					tail_.store(uniqueData, std::memory_order_release);

					succes = true;
					return;
				}

				succes = false;
			}


			~Queue()
			{
				while (numOfInQueue() > 0)
					pop()->release();

				std::free(dummy);
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
				bool firstTry = true;

				while (true)
				{
					UniqueData* head = head_.load(std::memory_order_acquire);
					UniqueData* tail = tail_.load(std::memory_order_acquire);

					UniqueData* tailNext = tail->next_.load(std::memory_order_acquire);

					if (head == tail)
					{
						if (head->state_ == UniqueData::State::Recorded)
						{
							head->state_ = UniqueData::State::Utilized;

							return head;
						}

						//Try update tail, if next != nullptr.
						if (tailNext != nullptr)
							tail_.compare_exchange_strong(tail, tailNext);

						return nullptr;
					}

					UniqueData* headNext = head->next_.load(std::memory_order_acquire);
					head_.store(headNext, std::memory_order_release);

					numOfInQueue_.fetch_sub(1, std::memory_order_release);

					if (head->state_ == UniqueData::State::Utilized)
					{
						if (!firstTry)
							return nullptr;

						firstTry = false;
						continue;
					}

					head->state_ = UniqueData::State::Utilized;

					return head;
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

							numOfInQueue_.fetch_add(1, std::memory_order_release);

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


			// returns the number of elements(UniqueData) in the queue.
			size_t numOfInQueue() noexcept
			{
				return numOfInQueue_.load(std::memory_order_acquire);
			}


		private:

			std::atomic<UniqueData*> head_;
			void* dummy;
			char padding1[GREEZEZ_CACHE_LINE_SIZE - (sizeof(std::atomic<UniqueData*>) + sizeof(void*))]{};

			std::atomic<UniqueData*> tail_;
			char padding2[GREEZEZ_CACHE_LINE_SIZE - sizeof(std::atomic<UniqueData*>)]{};

			std::atomic_size_t numOfInQueue_;

		};
	}


}

#endif // !GREEZEZ_MPSCQUEUE_HPP