module invalid.lifetime

capability Token {}

function escape(value: capability Token) -> capability Token
authority { result derives_from value }
{
    region temporary {
        return value
    }
}
