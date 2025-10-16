/*

DISKSPD

Copyright(c) Microsoft Corporation
All rights reserved.

MIT License

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED *AS IS*, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

*/

#pragma once
#include <Windows.h>

//
// OverlappedQueue is a simple class that implements a queue for OVERLAPPED
// elements
//

typedef struct {
  OVERLAPPED overlapped;
  BYTE *buffer;
  ULONGLONG index;
  ULONGLONG request_data_length;
  ULONGLONG file_id;
} NEW_OVERLAPPED;

class OverlappedQueue {
public:
  OverlappedQueue(void);

  void Add(NEW_OVERLAPPED *pOverlapped);
  bool IsEmpty(void) const;
  NEW_OVERLAPPED *Remove(void);
  NEW_OVERLAPPED *Get(void);
  size_t GetCount() const;

private:
  NEW_OVERLAPPED *_pHead;
  NEW_OVERLAPPED *_pTail;
  size_t _cItems;
};