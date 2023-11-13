arr=("30us" "35us" "40us" "45us" "50us" )
ratios=("0.025" "0.05" "0.07")

for ratio in ${ratios[@]}; do
	for time_slice in ${arr[@]}; do
		echo "RUN $ratio $time_slice"
		sudo bazel-bin/experiments/scripts/shinjuku.par ghost $ratio $time_slice | tee ghost_${ratio}_${time_slice}.out
		echo "Done ghost_${ratio}_${time_slice}.out"
	done
done	

