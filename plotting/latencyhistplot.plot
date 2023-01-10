#
set title "Latency histogram plot"
set term png  size 1600,720
set output 'latencyhistogram.png'
#set term x11 size 1600,720

set style fill solid
set ytics nomirror
set grid ytics
set y2tics        
set y2label "Cumulative %"
#set xtics 0, 300
#set xrange [0: 5000]

set ylabel "Occurence"
set xlabel "Latency (us)"


plot "latencydata.txt" using 1:2  with boxes title "Latency histogram", \
		"latencydata.txt" using 1:3  with lines lw 2 title "Cumulative distribution" axes x1y2
