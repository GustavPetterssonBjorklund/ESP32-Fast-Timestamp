#include <fast_timestamp.h>
using namespace fasttime;

void setup()
{
    Serial.begin(115200);
}

void loop()
{
    // Measure elapsed time for a code block
    Timestamp start = Timestamp::now();

    // Simulate workload
    delay(100);

    Timestamp end = Timestamp::now();
    uint64_t elapsed_cycles = cycles_between(start, end);
    Serial.print("Elapsed cycles: ");
    Serial.println(elapsed_cycles);
    Serial.print("Elapsed time (us): ");
    Serial.println(cycles_to_us(elapsed_cycles));
    delay(1000);
}
