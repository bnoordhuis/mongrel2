export LD_PRELOAD=$(dirname $0)/../deps/zeromq-2.0.8/src/.libs/libzmq.so

for i in tests/*_tests
do
    if test -f $i
    then
        ./$i
    fi
done

