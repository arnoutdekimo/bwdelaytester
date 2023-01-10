# Create a Plot or User and System CPU Usage, update regularly
#
set title "bwdelaytester results"
#set term png  size 1600,720
#set output 'output.png'
set term x11 size 1600,720

set ytics nomirror
set yrange [0:2000]
set grid ytics
set y2tics        
set y2label "Latency (ms)"
set y2range [0:200]

set ylabel "Mbps"


plot "< tail -n 200 data.dat" using 0:(($1*8)/1000000)  with lines lw 1 title "Mbps", \
     "< tail -n 200 data.dat" using 0:(($3*1016*8*10)/1000000)  with lines lw 1 title "Dropped Mbps (est)", \
     "< tail -n 200 data.dat" using 0:($5/1000)  with lines  lw 1 linecolor 3 title "Min delay" axes x1y2, \
     "< tail -n 200 data.dat" using 0:($6/1000)  with lines  lw 1 linecolor 4 title "Max delay" axes x1y2, \
	 "< tail -n 200 data.dat" using 0:($7/1000)  with lines  lw 1 linecolor 5 title "Avg delay" axes x1y2
	 
bind "ctrl-x" "unset output; exit gnuplot"

while (1) {
    replot
	pause 0.1
}
