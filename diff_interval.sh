arr=("0.025" "0.05" "0.07" "0.08" "0.09" "0.1" )

for time_slice in ${arr[@]}; do
	echo "RUN $time_slice"
	sudo bazel-bin/experiments/scripts/shinjuku.par ghost $time_slice 30us | tee ghost_$time_slice.out
	echo "Done ghost_$time_slice.out"
	sudo bazel-bin/experiments/scripts/shinjuku.par cfs $time_slice 30us | tee cfs_$time_slice.out
	echo "Done cfs_$time_slice.out"
done 

