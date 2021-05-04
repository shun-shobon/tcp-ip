#include "test.h"
#include "util.h"

int main(void) {
  debugf("Hello, World!");
  debugdump(test_data, sizeof(test_data));

  return 0;
}
