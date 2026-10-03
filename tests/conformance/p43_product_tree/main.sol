module conformance.p43_product_tree

record Inner { text: Text, count: Int64 }
record Outer { inner: Inner, other: Text, flag: Bool }

@entry
public function first() -> Int64 effects { pure } {
    let outer = Outer { inner = Inner { text = "", count = 40 }, other = "tree", flag = true }
    let copied = outer
    let nested = copied.inner.text
    let equal = outer == copied
    let unequal = copied != Outer { inner = Inner { text = "", count = 40 }, other = "other", flag = true }
    return if equal && unequal && nested == "" && copied.inner.count == 40 && copied.flag { 42 } else { 0 }
}

function second() -> Int64 effects { pure } { return 0 }
