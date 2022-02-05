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

		struct DataBlockHeader
		{
			std::atomic_size_t numOfAcquired;
			char padding1[GREEZEZ_CACHE_LINE_SIZE - sizeof(std::atomic_size_t)];

			size_t offset;
			size_t size;
			char padding2[GREEZEZ_CACHE_LINE_SIZE - (sizeof(size_t) * 2)];
		};

		

		DataBlock(size_t size, bool& success) noexcept :
			data_(nullptr)
		{
			data_ = std::malloc((2 * GREEZEZ_CACHE_LINE_SIZE) + size);

			if (data_ == nullptr)
			{
				success = false;
				return;
			}

			DataBlockHeader* header = static_cast<DataBlockHeader*>(data_);

			header->numOfAcquired = 0;
			header->size = size;
			header->offset = 0;

			success = true;
		}

		~DataBlock()
		{
			std::free(data_);
		}


		void* acquire(size_t size) noexcept
		{
			DataBlockHeader* header = static_cast<DataBlockHeader*>(data_);

			if (size > (header->size - header->offset))
				return nullptr;

			void* ptr = static_cast<void*>(static_cast<uint8_t*>(data_) + (GREEZEZ_CACHE_LINE_SIZE * 2) + header->offset );
			header->offset += size;

			header->numOfAcquired.fetch_add(1, std::memory_order_release);

			return ptr;
		}


		void release() noexcept
		{
			static_cast<DataBlockHeader*>(data_) -> numOfAcquired.fetch_sub(1, std::memory_order_release);
		}


		void reset() noexcept
		{
			static_cast<DataBlockHeader*>(data_) -> offset = 0;
		}


		size_t size() noexcept
		{
			return static_cast<DataBlockHeader*>(data_)->size;
		}


	private:

		void* data_;
	};

	
	

	template<typename T>
	class Produser;

	template<typename T>
	class Consumer
	{

		friend class Producer;

	public:
		Consumer() noexcept 
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

		

	};



	template<typename T>
	class Produser
	{

	public:

		Produser(size_t dataBlockCount, size_t objectCountPerDataBlock, bool& success) noexcept :
			dataBlockList_()
		{
			
		}

		~Produser()
		{
			
		}

		template<typename ... ARGS>
		bool trySendTo(Consumer<T>& consumer, ARGS& ... args) noexcept 
		{}

		template<typename ... ARGS>
		bool sendTo(Consumer<T>& consumer, ARGS& ... args) noexcept
		{}


	private:

		details::List<DataBlock> dataBlockList_;
		
	};
	
}

#endif // !GREEZEZ_MPSCQUEUE_HPP