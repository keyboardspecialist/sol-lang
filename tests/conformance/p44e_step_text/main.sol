module conformance.p44e_step_text

record Box { text: Text }
record TaggedBox { tag: Int64, text: Text }
record Nest { inner: Box }
enum Word { blank, text(value: Text) }
enum Tree { node(value: Box) }

@entry
public function launch() -> Int64 effects { pure } {
    let text = "abc"
    let copy = text
    let box = Box { text = "abc" }
    let box_copy = box
    let word = Word.text("abc")
    let word_copy = word
    return if text == copy && box == box_copy && word == word_copy { 42 } else { 0 }
}

public function empty() -> Int64 effects { pure } {
    let text = ""
    let copy = text
    return if text == copy { 42 } else { 0 }
}

public function mismatch() -> Int64 effects { pure } {
    return if "abc" != "axc" { 42 } else { 0 }
}

public function length_mismatch() -> Int64 effects { pure } {
    return if "a" != "abc" { 42 } else { 0 }
}

public function identity() -> Int64 effects { pure } {
    let text = "abc"
    return if text == text { 42 } else { 0 }
}

public function late_mismatch() -> Int64 effects { pure } {
    return if "abc" != "abx" { 42 } else { 0 }
}

public function product_mismatch() -> Int64 effects { pure } {
    let left = TaggedBox { tag = 1, text = "abc" }
    let right = TaggedBox { tag = 2, text = "abc" }
    return if left != right { 42 } else { 0 }
}

public function sum_tag_mismatch() -> Int64 effects { pure } {
    let left = Word.blank
    let right = Word.text("abc")
    return if left != right { 42 } else { 0 }
}

public function nested() -> Int64 effects { pure } {
    let nest = Nest { inner = Box { text = "abc" } }
    let nest_copy = nest
    let tree = Tree.node(Box { text = "abc" })
    let tree_copy = tree
    return if nest == nest_copy && tree == tree_copy { 42 } else { 0 }
}
