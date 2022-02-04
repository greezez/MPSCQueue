#pragma once
#ifndef GREEZEZ_MPSCQUEUE_HPP
#define GREEZEZ_MPSCQUEUE_HPP

#ifndef GREEZEZ_CACHE_LINE_SIZE
#define GREEZEZ_CACHE_LINE_SIZE 64
#endif

#include <atomic>

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


			T* front() noexcept 
			{
				return head_;
			}
			
			
			T* current() noexcept 
			{
				return current_;
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
						return front();

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



	namespace mpsc
	{

		class Produser
		{
		public:
			Produser()
			{
			}

			~Produser()
			{
			}

		private:

			
		};



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
	}
}

#endif // !GREEZEZ_MPSCQUEUE_HPP