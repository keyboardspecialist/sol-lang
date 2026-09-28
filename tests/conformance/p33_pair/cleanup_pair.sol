module cleanup_affine_pair

record Pair {
    left: function() -> Int64 effects { pure },
    right: Text
}

function definite_left(left: function() -> Int64 effects { pure }) -> () effects { pure } {
    let pair = Pair { left = left, right = "right" }
    let moved = pair.left
}

function conditional_left(left: function() -> Int64 effects { pure }, choose: Bool) -> () effects { pure } {
    let pair = Pair { left = left, right = "right" }
    if choose {
        let moved = pair.left
    } else {
        let marker = false
    }
}

function repaired_left(left: function() -> Int64 effects { pure }) -> () effects { pure } {
    var pair = Pair { left = left, right = "right" }
    let moved = pair.left
    pair.left = moved
}

function moved_root(left: function() -> Int64 effects { pure }) -> () effects { pure } {
    let pair = Pair { left = left, right = "right" }
    let moved = pair
}
