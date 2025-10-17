#include <iostream>
#include <thread>
#include <chrono>
#include "../common/InOutputData/InputDataFromSHM.h"

class MyListener : public IInputDataListener {
public:
  void InputData(unsigned char *pData, std::uint64_t uiDataLength, void *pParam = NULL) override {
    std::cout << "Consumer got data ptr=" << static_cast<void*>(pData) << ", len=" << uiDataLength << std::endl;
    // just print first 8 bytes
    for (size_t i = 0; i < std::min<std::size_t>(uiDataLength, 8); ++i) std::cout << std::hex << (int)pData[i] << " ";
    std::cout << std::dec << std::endl;
  }
};

int main() {
  InputDataFromSHMInitParam param;
  param.shm_name = "/pcie_shm_ring";
  param.slot_count = 32;
  param.slot_size = 512 * 1024;
  param.create_if_missing = false;

  CInputDataFromSHM consumer;
  MyListener listener;
  consumer.SetInputDataListener(&listener);

  if (!consumer.Init(&param)) {
    std::cerr << "consumer Init failed" << std::endl;
    return 1;
  }
  if (!consumer.Start()) {
    std::cerr << "consumer Start failed" << std::endl;
    return 1;
  }

  // run for 5 seconds
  std::this_thread::sleep_for(std::chrono::seconds(5));

  consumer.Stop();
  return 0;
}
