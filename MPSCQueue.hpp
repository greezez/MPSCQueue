#pragma once
#ifndef GREEZEZ_MPSCQUEUE_HPP
#define GREEZEZ_MPSCQUEUE_HPP

#ifndef GREEZEZ_CACHE_LINE_SIZE
#define GREEZEZ_CACHE_LINE_SIZE 64
#endif

#define GREEZEZ_MIN_DATA_BOCK_SIZE GREEZEZ_CACHE_LINE_SIZE  

#include <atomic>

namespace greezez_mpsc
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
			T* emplaceAndUpdateCurrent(ARGS&& ... args) noexcept
			{
				if (head_ == nullptr)
				{
					if (emplaceFront(std::forward<ARGS>(args)...))
						return &head_;

					return nullptr;
				}

				Node* node = new(std::nothrow) Node(std::forward<ARGS>(args) ...);

				if (node == nullptr)
					return nullptr;

				node->next = current_->next;
				current_->next = node;
				current_ = node->next;

				size_++;

				return &node->item;
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

	class DataBlock
	{

	public:

		DataBlock(size_t size, bool& success) noexcept :
			numOfAcquired_(0), data_(nullptr), offset_(0), size_(size)
		{
			data_ = std::malloc(size);

			if (data_ == nullptr)
			{
				success = false;
				return;
			}

			success = true;
		}


		~DataBlock()
		{
			std::free(data_);
		}


		void* acquire(size_t size) noexcept
		{
			if (size > (size_ - offset_))
				return nullptr;

			void* prt = static_cast<void*>(static_cast<uint8_t*>(data_) + offset_);
			offset_ += size;

			numOfAcquired_.fetch_add(1, std::memory_order_release);

			return prt;
		}


		void release() noexcept
		{
			numOfAcquired_.fetch_sub(1, std::memory_order_release);
		}


		size_t size() noexcept
		{
			return size_;
		}


	private:

		std::atomic_size_t numOfAcquired_;
		char padding[GREEZEZ_CACHE_LINE_SIZE - sizeof(std::atomic_size_t)]{};

		void* data_;
		size_t offset_;
		size_t size_;
	};

	enum class DataState : uint8_t
	{
		Write = 0,
		Read
	};

	struct DataHeader
	{
		DataState state;

		DataBlock* dataBlock;
		std::atomic<DataHeader*> next;
	};

	

	template<typename T>
	class Produser;

	template<typename T>
	class Consumer
	{

		friend class Producer;

	public:
		Consumer() noexcept :
			head_(nullptr), tail_(nullptr), dummy_{DataState::Read, nullptr, nullptr}
		{
		}

		~Consumer()
		{
		}

		void recive() noexcept
		{}

	private:

		void send() noexcept 
		{}

	private:

		std::atomic<DataHeader*> head_;
		char padding1[GREEZEZ_CACHE_LINE_SIZE - sizeof(std::atomic<DataHeader*>)]{};

		std::atomic<DataHeader*> tail_;
		char padding2[GREEZEZ_CACHE_LINE_SIZE - sizeof(std::atomic<DataHeader*>)]{};

		DataHeader dummy_;

	};



	template<typename T>
	class Produser
	{

	public:

		Produser(size_t dataBlockCount, size_t objectCountPerDataBlock, bool& success) noexcept :
			dataBlockList_()
		{
			for (size_t i = 0; i < dataBlockCount; i++)
			{
				bool allocSuccess = false;

				if (!dataBlockList_.emplaceFront(GREEZEZ_MIN_DATA_BOCK_SIZE * objectCountPerDataBlock, allocSuccess) or !allocSuccess)
				{
					dataBlockList_.clear();
					success = false;
					return;
				}
			}

			dataBlockList_.resetCurrent();
			success = true;
		}

		~Produser()
		{
			dataBlockList_.clear();
		}

		template<typename ... ARGS>
		bool trySendTo(Consumer<T>& consumer, ARGS& ... args) noexcept 
		{}

		template<typename ... ARGS>
		bool sendTo(Consumer<T>& consumer, ARGS& ... args) noexcept
		{}

	private:



	private:

		details::List<DataBlock> dataBlockList_;
	};
	

	


		
	
}

#endif // !GREEZEZ_MPSCQUEUE_HPP