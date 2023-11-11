arr=("0.005" "0.075" "0.01" "0.015" "0.02" "0.025" "0.03" "0.035" "0.04" "0.045" "0.05" )

for time_slice in ${arr[@]}; do
	echo "RUN $time_slice"
	sudo bazel-bin/experiments/scripts/shinjuku.par ghost $time_slice 30us | tee ghost_$time_slice.out
	echo "Done ghost_$time_slice.out"
	sudo bazel-bin/experiments/scripts/shinjuku.par cfs $time_slice 30us | tee cfs_$time_slice.out
	echo "Done cfs_$time_slice.out"
done 

