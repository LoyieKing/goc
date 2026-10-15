package main

func callmany(s int64) int64
func deep(n int64) int64

func main() {
	println(callmany(10))
	ch := make(chan int64)
	go func() { ch <- deep(500) }()
	println(<-ch)
}
