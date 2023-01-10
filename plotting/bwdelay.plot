#
set title "BW delay graph"
set term png  size 1600,720
set output 'bwoutput.png'
#set term x11 size 1600,720

set ytics nomirror
set grid ytics
set y2tics        
set y2label "Loss %"
set logscale y2 10

set ylabel "Latency (ms)"
set xlabel "Mbps"


plot "bwdelaydat.txt" using 1:($3/1000)  with linespoints lw 1 title "Minimum delay", \
     "bwdelaydat.txt" using 1:($4/1000)  with linespoints lw 1 title "Maximum delay", \
     "bwdelaydat.txt" using 1:($5/1000)  with linespoints lw 1 title "Average delay", \
     "bwdelaydat.txt" using 1:2  with linespoints lw 2 title "Loss percent" axes x1y2
	
#bind "ctrl-x" "unset output; exit gnuplot"

#while (1) {
#	pause 0.1
#}
