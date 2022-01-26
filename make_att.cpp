#include <cmath>
#include <cstdio>
#include <cstdio>

float attenuation(int decibels)
{
    return pow(10, decibels / 20.0);
}

int main()
{
    for(unsigned int i = 0; i < 16; i++) {
        float volume = 1;
        if(i & 1)
            volume *= attenuation(-2);
        if(i & 2)
            volume *= attenuation(-4);
        if(i & 4)
            volume *= attenuation(-8);
        if(i & 8)
            volume *= attenuation(-16);
        printf("%d\n", int(256 * volume));
    }
}
