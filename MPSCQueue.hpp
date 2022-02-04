#pragma once
#ifndef GREEZEZ_MPSCQUEUE_HPP
#define GREEZEZ_MPSCQUEUE_HPP

#ifndef GREEZEZ_CACHE_LINE_SIZE
#define GREEZEZ_CACHE_LINE_SIZE 64
#endif

#define GREEZEZ_MIN_DATA_BOCK_SIZE GREEZEZ_CACHE_LINE_SIZE  

#include <atomic>

namespace greezez_mpscqueue
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
						return &front();

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



		class Data
		{

		public:

			Data(size_t size, bool& success) noexcept :
				data_(nullptr), offset_(0), size_(size)
			{
				data_ = std::malloc(size);

				if (data_ == nullptr)
				{
					success = false;
					return;
				}

				success = true;
			}


			~Data()
			{
				std::free(data_);
			}


			size_t size() noexcept
			{
				return size_;
			}


		private:

			void* data_;
			size_t offset_;
			size_t size_;
		};



		struct DataHeader
		{
			std::atomic<DataHeader*> next;
		};



		constexpr size_t DataHeaderSize = sizeof(DataHeader);
	}



	template<typename T>
	class Consumer
	{
	public:
		Consumer()
		{
		}

		~Consumer()
		{
		}

	private:

	};



	template<typename T>
	class Produser
	{

	public:

		Produser(size_t dataBlockCount, size_t maxDataBlockCount, size_t objectCountPerDataBlock, bool& success) noexcept :
			dataList_(), maxDataBlockCount_(maxDataBlockCount)
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
		}

		bool trySend(Consumer<T>& consumer, T& item) noexcept {}

	private:

		details::List<details::Data> dataBlockList_;

		size_t maxDataBlockCount_;
	};
	

	


		
	
}

#endif // !GREEZEZ_MPSCQUEUE_HPP