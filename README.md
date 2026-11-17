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



1. Cegiełki: kodeki jednego bloku

Pierwsza część pliku to funkcje, które nic nie wiedzą o wątkach ani o plikach. Biorą jeden blok bajtów i zwracają skompresowany albo odwrotnie:

rleCompress / rleDecompress: RLE.
huffCompress / huffDecompress: Huffman (opisany w poprzedniej odpowiedzi).
compressBlock i decompressBlock: przełączniki wybierające algorytm na podstawie Algo.

Dzięki temu równoległość da się dołożyć bez zmian w algorytmach, bo każdy blok jest niezależny.

2. Kolejka i pula wątków (BoundedQueue, compressBlocks)

To model producent–konsument:

Producent to wątek główny. Czyta kolejne bloki z pliku (sekwencyjnie, zgodnie z założeniami) i wkłada je do kolejki jako Task{idx, data}.
Konsumenci to p wątków roboczych. Każdy robi q.pop(t), kompresuje blok i zapisuje wynik w results[t.idx].

Kolejka jest ograniczona (pojemność 2p). Gdy jest pełna, producent czeka na notFull_ i nie wczyta całego pliku do RAM-u naraz. Gdy jest pusta, konsumenci czekają na notEmpty_. Po wrzuceniu ostatniego bloku wątek główny woła q.close(), a pop zwraca wtedy false i wątki kończą pracę.

Kolejność w pliku wyjściowym jest zachowana dzięki indeksowi: wątki kończą w dowolnej kolejności, ale każdy pisze do własnego slotu results[idx]. Różne sloty to różne elementy wektora, więc nie trzeba mutexu.

Błędy z wątków roboczych są łapane i przekazywane do wątku głównego przez std::exception_ptr. Przy awarii kolejka jest opróżniana, żeby producent się nie zawiesił.

3. Format pliku (serialize)
   "ZSSK" | algo (1 B) | N (8 B) | B (4 B) | liczba bloków (8 B) | rozmiary bloków (4 B każdy) | dane bloków

Indeks rozmiarów pozwala przy dekompresji obliczyć, gdzie zaczyna się każdy blok, bez czytania poprzednich. Dzięki temu dekompresja też jest równoległa. Rozmiar oryginalnego bloku nie jest zapisany: to zawsze B, a ostatni to reszta N − i·B.

4. Dekompresja (decompressBuffer)
   Sprawdza nagłówek (magic, algorytm, spójność nblocks == ⌈N/B⌉, sumę rozmiarów).
   Liczy tablicę przesunięć off[i] jako sumy prefiksowe rozmiarów.
   Alokuje bufor wyjściowy out o rozmiarze N.
   Uruchamia p wątków, które biorą kolejne indeksy z licznika atomowego (nextIdx.fetch_add(1)). Każdy dekompresuje blok i prosto na out + i·B.

Tu producent nie jest potrzebny, bo cały plik jest już w pamięci. Licznik atomowy wystarcza do rozdawania pracy, a bloki wyjściowe są rozłączne, więc wątki sobie nie przeszkadzają.

5. Przepływ przy zssk compress

cmdCompress otwiera plik, odczytuje jego rozmiar N, ustawia B = 2^k i tworzy lambdę BlockReader, która czyta z pliku jeden blok. Wywołuje compressBlocks (kolejka plus wątki), potem serialize i zapis do pliku. Na końcu wypisuje N, rozmiar wyniku, R i czas.

6. Tryb bench

To sedno badań z opisu projektu. Plik wczytywany jest raz do pamięci, a potem w pętlach:

po algorytmach, po k od bmin do bmax, po liczbach wątków,
każdą konfigurację mierzy reps razy (std::chrono::steady_clock) i uśrednia,
liczy R jako containerSize / N i S(p) jako T(B,1) / T(B,p),
po pierwszym pomiarze w danej serii robi pełną dekompresję i porównuje sumę FNV-1a z oryginałem (kolumna „OK“),
zapisuje wiersze do CSV.

Czas mierzy tylko compressBuffer (sama kompresja z pamięci), więc bez odczytu i zapisu pliku. Pętla przerywa się, gdy B przekroczy rozmiar pliku, bo kolejne wartości dawałyby identyczny wynik (jeden blok).

Na końcu program odpowiada na oba problemy z opisu:

decyzyjny: wypisuje pary (B, p) z R ≤ R0 i T ≤ T0 albo „NIE“,
optymalizacyjny: minimalizuje w·R/R_min + (1−w)·T/T_min, czyli ważoną sumę znormalizowanych R i T. Ograniczenie pamięci p·B filtruje kandydatów przez --memlimit.
7. selftest

Dla zestawu przypadków brzegowych (pusty plik, 1 bajt, niepełny blok, jeden powtarzany znak, losowe dane, rozkład Fibonacciego) i dla k = 10, 12, 16 sprawdza dwie rzeczy:

dekompresja odtwarza oryginał,
wynik dla p = 2, 4, 8 jest bajt w bajt identyczny z wynikiem dla p = 1.

Drugi punkt odpowiada wymaganiu z opisu: „wynik wersji równoległej musi być identyczny z sekwencyjną“.

Mogę też rozrysować to na diagramie (producent, kolejka, wątki, sloty wyników) albo przejść linia po linii przez wybrany fragment, np. akumulator bitów w huffCompress.
