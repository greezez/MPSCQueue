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

	}



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
			data_ = std::malloc(HeaderSize + blockCount* BlockSize);

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



	class Producer;
	class Consumer;
	


	class UniqueData
	{

		friend class Producer;
		friend class Consumer;

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


		UniqueData() noexcept :
			state_(State::Recorded), allocType_(AllocType::Pool), offset_(0),
			data_(nullptr), next_(nullptr)
		{}


		UniqueData(const UniqueData&) = delete;
		UniqueData& operator=(const UniqueData& other) = delete;


		UniqueData(UniqueData&& other) noexcept :
			state_(other.state_), allocType_(other.allocType_), offset_(other.offset_),
			data_(other.data_), next_(nullptr)
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


		template<size_t Size>
		static UniqueData* make() noexcept
		{
			void* ptr = std::malloc(sizeof(UniqueData) + Size);

			if (ptr != nullptr)
			{
				UniqueData* uniqueData = new(ptr) UniqueData(UniqueData::State::Recorded, UniqueData::AllocType::Heap, 0, ptr);
				return uniqueData;
			}

			return nullptr;
		}

		
		template<typename T>
		static UniqueData* make()
		{
			return UniqueData::make<sizeof(T)>();
		}


		void* raw() noexcept
		{
			return static_cast<void*>(static_cast<uint8_t*>(data_) + sizeof(UniqueData));
		}


		template<typename T>
		T* get() noexcept
		{
			return static_cast<T*>(raw());
		}


		template<typename T, typename ... ARGS>
		T* emplace(ARGS&& ... args) noexcept
		{
			return new(raw()) T(std::forward<ARGS>(args) ...);
		}


		void release() noexcept
		{
			if (!isValid())
				return;

			if (allocType_ == AllocType::Heap)
			{
				std::free(data_);
				return;
			}

			Data::Header* dataHeader = reinterpret_cast<Data::Header*>
				(static_cast<uint8_t*>(data_) - (Data::HeaderSize + offset_ * Data::BlockSize));

			dataHeader->numOfAcquired.fetch_sub(1, std::memory_order_release);
		}


		bool isValid() noexcept
		{
			return data_ != nullptr;
		}


	private:

		UniqueData(State state, AllocType allocType, uint16_t offset, void* data) noexcept :
			state_(state), allocType_(allocType), offset_(offset),
			data_(data), next_(nullptr)
		{}


		UniqueData(nullptr_t, void* data) noexcept :
			state_(State::Utilized), allocType_(AllocType::Heap), offset_(0),
			data_(data), next_(nullptr)
		{}


	private:

		State state_;
		AllocType allocType_;

		uint16_t offset_;

		void* data_;
		std::atomic<UniqueData*> next_;
	};



	class Producer
	{

	public:

		Producer(size_t dataPoolSize, size_t dataBlockCount, bool& success) noexcept :
			dataBlockCount_(dataBlockCount), dataList_()
		{
			bool allocSuccess = false;

			for (size_t i = 0; i < dataPoolSize; i++)
			{
				if (!dataList_.emplaceFront(dataBlockCount, allocSuccess) or !allocSuccess)
				{
					dataList_.clear();
					success = false;
					return;
				}
			}

			success = true;
		}


		~Producer()
		{
			dataList_.clear();
		}
		

		template<typename T>
		UniqueData* tryAcquire() noexcept
		{
			constexpr size_t blockCount = (sizeof(UniqueData) + sizeof(T) <= Data::BlockSize) 
				? 1 : ((sizeof(UniqueData) + sizeof(T)) / Data::BlockSize) + 1;

			bool firstTry = true;

			while (true)
			{
				Data& data = dataList_.current();
				uint16_t currentOffset = static_cast<uint16_t>(data.offset());

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

		
		template<typename T>
		UniqueData* acquire() noexcept
		{
			UniqueData* uniqueData = tryAcquire<T>();

			if (uniqueData != nullptr)
				return uniqueData;

			bool succes = false;
			if (!dataList_.emplaceAndUpdateCurrent(dataList_.front().blockCount(), succes) or !succes)
				return nullptr;

			return tryAcquire<T>();
		}


		size_t size() noexcept
		{
			return dataList_.size();
		}


	private:

		size_t dataBlockCount_;
		details::List<Data> dataList_;
	};



	class Consumer
	{

	public:

		Consumer(bool& succes) noexcept :
			head_(nullptr), tail_(nullptr), numOfData_(0)
		{
			void* ptr = std::malloc(sizeof(UniqueData));

			if (ptr != nullptr)
			{
				UniqueData* uniqueData = new(ptr) UniqueData(nullptr, ptr);

				head_.store(uniqueData, std::memory_order_relaxed);
				tail_.store(uniqueData, std::memory_order_release);

				succes = true;
				return;
			}

			succes = false;
		}


		~Consumer()
		{
		}


		UniqueData* pop() noexcept
		{
		//next...
		}


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

						numOfData_.fetch_add(1, std::memory_order_release);

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


		size_t numOfData() noexcept
		{
			return numOfData_.load(std::memory_order_acquire);
		}

	private:

		std::atomic<UniqueData*> head_;
		char padding1[GREEZEZ_CACHE_LINE_SIZE - sizeof(std::atomic<UniqueData*>)]{};

		std::atomic<UniqueData*> tail_;
		char padding2[GREEZEZ_CACHE_LINE_SIZE - sizeof(std::atomic<UniqueData*>)]{};

		std::atomic_size_t numOfData_;

	};
}

#endif // !GREEZEZ_MPSCQUEUE_HPP