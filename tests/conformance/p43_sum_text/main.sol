module conformance.p43_sum_text

enum Echo { blank, word(value: Text), number(value: Int64) }

@entry
public function first() -> Int64 effects { pure } {
    let word = Echo.word("same")
    let word_copy = word
    let same_content = Echo.word("same")
    let different_content = Echo.word("other")
    let blank = Echo.blank
    let number = Echo.number(7)
    return if word == word_copy && word == same_content && word != different_content
        && word != blank && word != number { 42 } else { 0 }
}

function second() -> Int64 effects { pure } { return 0 }
