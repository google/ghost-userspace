arr=("60us" "65us" "70us" "80us")
ratios=("0.08" "0.09" "0.1")

for ratio in ${ratios[@]}; do
	for time_slice in ${arr[@]}; do
		echo "RUN $ratio $time_slice"
		sudo bazel-bin/experiments/scripts/shinjuku.par ghost $ratio $time_slice | tee ghost_${ratio}_${time_slice}.out
		echo "Done ghost_${ratio}_${time_slice}.out"
	done
done	

