// MIT License
//
// Copyright(c) 2021 greezez
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this softwareand associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright noticeand this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.



#pragma once
#ifndef GREEZEZ_LOCKFREE_DETAILS_HPP
#define GREEZEZ_LOCKFREE_DETAILS_HPP


#include <atomic>


#ifndef GREEZEZ_CACHE_LINE_SIZE
#define GREEZEZ_CACHE_LINE_SIZE 64
#endif



namespace Greezez
{

	namespace Details
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



		struct alignas(GREEZEZ_CACHE_LINE_SIZE) MemoryBlockHeader
		{
			std::atomic_size_t isFull;
			size_t offset;
			char padding1[GREEZEZ_CACHE_LINE_SIZE - (sizeof(std::atomic_size_t) + sizeof(size_t))]{};

			std::atomic_size_t numOfAcquired;
			char padding2[GREEZEZ_CACHE_LINE_SIZE - sizeof(std::atomic_size_t)]{};

			void release()
			{
				numOfAcquired.fetch_sub(1, std::memory_order_release);

				if (numOfAcquired.load(std::memory_order_acquire) != 0)
					return;

				offset = 0;
				isFull.store(0, std::memory_order_release);
			}
		};



		template<size_t TNumOfChunk, size_t TChunkSize>
		class MemoryBlock
		{

		public:

			static constexpr size_t Size = TNumOfChunk * TChunkSize;


			MemoryBlock(bool& success) noexcept :
				header_(nullptr), data_(nullptr)
			{
				data_ = std::malloc(sizeof(MemoryBlockHeader) + Size);

				if (data_ == nullptr)
				{
					success = false;
					return;
				}

				header_ = static_cast<MemoryBlockHeader*>(data_);

				header_->numOfAcquired.store(0, std::memory_order_release);
				header_->isFull = 0;
				header_->offset = 0;

				success = true;
			}


			~MemoryBlock()
			{
				std::free(data_);
			}


			void release() noexcept
			{
				while (true)
				{
					if (header_->numOfAcquired.load(std::memory_order_acquire) != 0)
						continue;

					std::free(data_);
					data_ = nullptr;
					return;
				}
			}


			void* acquire(size_t numOfBlock) noexcept
			{
				if (header_->isFull.load(std::memory_order_acquire) == 1)
					return nullptr;

				if (numOfBlock > (TNumOfChunk - header_->offset))
				{
					header_->isFull.store(1, std::memory_order_release);
					return nullptr;
				}

				void* ptr = static_cast<void*>(static_cast<uint8_t*>(data_) + sizeof(MemoryBlockHeader) + header_->offset * TChunkSize);
				header_->offset += numOfBlock;

				if (TNumOfChunk == header_->offset)
					header_->isFull.store(1, std::memory_order_release);

				header_->numOfAcquired.fetch_add(1, std::memory_order_release);

				return ptr;
			}


			bool empty()
			{
				return header_->numOfAcquired.load(std::memory_order_acquire) == 0;
			}

			size_t offset() noexcept
			{
				return header_->offset;
			}


			constexpr size_t NumOfChunk() noexcept
			{
				return TNumOfChunk;
			}


			/*	void dump()
				{
					std::cout << "ChunkCount: " << chunkCount() << " Offset: " << offset() << " isFull: " << header_->isFull.load(std::memory_order_acquire) << std::endl;
					std::cout << "Num: " << header_->numOfAcquired.load(std::memory_order_acquire) << std::endl;

					void* dt = static_cast<void*>(static_cast<uint8_t*>(data_) + sizeof(Header));


					for (uint64_t block64bits = 0; block64bits < (chunkCount() * ChunkSize) / 8; block64bits++)
					{

						uint64_t u64 = *(static_cast<uint64_t*>(dt) + block64bits);

						std::cout << "64b " << block64bits << ":  \t";

						int st = 0;

						for (size_t bitOffset = 0; bitOffset < 64; bitOffset++)
						{
							if (bool((0x8000000000000000 >> bitOffset) & u64))
								std::cout << "1";
							else
								std::cout << "0";
							st++;

							if (st != 8)
								continue;

							std::cout << " ";
							st = 0;
						}

						std::cout << std::endl;
					}
					std::cout << std::endl;
				}*/


		private:

			MemoryBlockHeader* header_;
			void* data_;
		};



		enum class UniqueRawDataAllocType : uint32_t
		{
			Pool = 1,
			Heap,

			// needed for dummy
			None
		};



		struct UniqueRawDataHeader
		{
			uint32_t offset;
			UniqueRawDataAllocType allocType;
		};



		template<size_t TNumOfChunk, size_t TChunkSize>
		class AllocatorBase
		{

			using MemoryBlockT = MemoryBlock<TNumOfChunk, TChunkSize>;

		public:

			AllocatorBase(size_t numOfMemoryBlocks, bool& success) noexcept :
				memoryBlockList_()
			{
				bool allocSuccess = false;

				for (size_t i = 0; i < numOfMemoryBlocks; i++)
				{
					if (!memoryBlockList_.emplaceFront(allocSuccess) or !allocSuccess)
					{
						memoryBlockList_.clear();
						success = false;
						return;
					}
				}

				success = true;
			}


			~AllocatorBase()
			{
				memoryBlockList_.clear();
			}


			void* tryAcquire(size_t size) noexcept
			{
				size_t numOfChunk = ((sizeof(UniqueRawDataHeader) + size) <= TChunkSize)
					? 1 : ((sizeof(UniqueRawDataHeader) + size) / TChunkSize) + 1;

				bool firstTry = true;

				while (true)
				{
					MemoryBlockT& data = memoryBlockList_.current();
					uint32_t lastOffset = static_cast<uint32_t>(data.offset());

					void* ptr = data.acquire(numOfChunk);

					if (ptr != nullptr)
					{
						UniqueRawDataHeader* dataHeader = static_cast<UniqueRawDataHeader*>(ptr);

						dataHeader->offset = sizeof(MemoryBlockHeader) + (lastOffset * TChunkSize);
						dataHeader->allocType = UniqueRawDataAllocType::Pool;

						return static_cast<void*>(static_cast<uint8_t*>(ptr) + sizeof(UniqueRawDataHeader));
					}

					if (!firstTry)
						return nullptr;

					firstTry = false;

					memoryBlockList_.updateCurrent();
				}
			}


			void* acquire(size_t size) noexcept
			{
				void* ptr = tryAcquire(size);

				if (ptr != nullptr)
					return ptr;

				bool succes = false;
				if (!memoryBlockList_.emplaceAndUpdateCurrent(succes) or !succes)
					return nullptr;

				return tryAcquire(size);
			}


			void* TryAcquireFromHeap(size_t size, uint32_t data = 0) noexcept
			{
				void* ptr = std::malloc(sizeof(UniqueRawDataHeader) + size);

				if (ptr == nullptr)
					return nullptr;
			
				UniqueRawDataHeader* dataHeader = static_cast<UniqueRawDataHeader*>(ptr);

				dataHeader->offset = data;
				dataHeader->allocType = UniqueRawDataAllocType::Heap;

				return static_cast<void*>(static_cast<uint8_t*>(ptr) + sizeof(UniqueRawDataHeader));
			}

		private:

			List<MemoryBlockT> memoryBlockList_;

		};



		void allocationDataFree(void* ptr)
		{
			if (ptr == nullptr)
				return;

			UniqueRawDataHeader* uniqueRawDataHeader = reinterpret_cast<UniqueRawDataHeader*>(static_cast<uint8_t*>(ptr) - sizeof(UniqueRawDataHeader));

			if (uniqueRawDataHeader->allocType == UniqueRawDataAllocType::Pool)
			{
				MemoryBlockHeader* header = reinterpret_cast<MemoryBlockHeader*>(static_cast<uint8_t*>(ptr) - uniqueRawDataHeader->offset);

				header->release();

				return;
			}


			if (uniqueRawDataHeader->allocType == UniqueRawDataAllocType::Heap)
			{
				std::free(static_cast<void*>(uniqueRawDataHeader));
				return;
			}
		}



		template<typename THeader>
		class UniqueDataBase
		{

		public:

			UniqueDataBase() noexcept :
				data_(nullptr)
			{
			}


			UniqueDataBase(const UniqueDataBase&) = delete;
			UniqueDataBase& operator=(const UniqueDataBase& other) = delete;


			UniqueDataBase(const UniqueDataBase&& other) noexcept :
				data_(other.data_)
			{
				other.data_ = nullptr;
			}


			UniqueDataBase& operator=(const UniqueDataBase&& other) noexcept
			{
				if (&other == this)
					return *this;

				data_ = other.data_;
				other.data_ = nullptr;

				return *this;
			}


			~UniqueDataBase()
			{
				release();
			}

			
			void* raw() noexcept
			{
				return static_cast<void*>(static_cast<uint8_t*>(data_) + sizeof(THeader));
			}


			template<typename T>
			void get() noexcept
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
				allocationDataFree(data_);
			}


			template<typename T>
			void destructAndRelease() noexcept
			{
				static_cast<T*>(static_cast<uint8_t*>(data_) + sizeof(THeader))->~T();
				
				release();
			}


			bool valid() noexcept
			{
				return data_ != nullptr;
			}

		private:

			template<typename T>
			friend void setUniqueDataBase(UniqueDataBase<T>& uniqueData, void* data) noexcept;

		private:

			void* data_;

		};



		template<typename T>
		void setUniqueDataBase(UniqueDataBase<T>& uniqueData, void* data) noexcept
		{
			uniqueData.data_ = data;
		}



		template<typename THeader, size_t TNumOfChunk, size_t TChunkSize>
		class UniqueDataPoolBase
		{

		public:

			using Allocator = AllocatorBase<TNumOfChunk, TChunkSize>;
			using UniqueData = UniqueDataBase<THeader>;


			UniqueDataPoolBase(Allocator& allocator) noexcept :
				externalAllocator_(true), allocator_(&allocator)
			{
			}


			UniqueDataPoolBase(size_t numOfMemoryBlocks, bool& success) noexcept :
				externalAllocator_(false), allocator_(nullptr)
			{
				allocator_ = new(std::nothrow) Allocator(numOfMemoryBlocks, success);

				if (allocator_ == nullptr or !success)
					success = false;
			}


			~UniqueDataPoolBase()
			{
				if (externalAllocator_)
					return;

				delete allocator_;
			}


			bool tryAcquire(UniqueData& uniqueData, size_t size) noexcept
			{
				void* ptr = allocator_->tryAcquire(sizeof(THeader) + size);

				if (ptr == nullptr)
					return false;

				new(ptr) THeader;

				setUniqueDataBase(uniqueData, ptr);

				return true;
			}


			bool acquire(UniqueData& uniqueData, size_t size) noexcept
			{
				void* ptr = allocator_->acquire(sizeof(THeader) + size);

				if (ptr == nullptr)
					return false;

				new(ptr) THeader;

				setUniqueDataBase(uniqueData, ptr);

				return true;
			}


			bool TryAcquireFromHeap(UniqueData& uniqueData, size_t size, uint32_t data = 0) noexcept
			{
				void* ptr = allocator_->TryAcquireFromHeap(sizeof(THeader) + size);

				if (ptr == nullptr)
					return false;

				new(ptr) THeader;

				setUniqueDataBase(uniqueData, ptr);

				return true;
			}

		private:

			bool externalAllocator_;
			Allocator* allocator_;

		};



	}

}
		

#endif // !GREEZEZ_LOCKFREE_DETAILS_HPP