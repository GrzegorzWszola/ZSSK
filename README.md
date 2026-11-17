
mkdir build

cd build

cmake ..

make --build .

./zssk xxx...

zssk compress   in.bmp out.zssk --algo huff --block 16 --threads 4
zssk decompress out.zssk in2.bmp --threads 4
zssk bench plik.bin --algo both --threads 1,2,4,8 --bmin 10 --bmax 26 --reps 5 --csv wyniki.csv
zssk bench plik.bin --R0 0.5 --T0 200 --memlimit 512 --w 0.5
zssk selftest
