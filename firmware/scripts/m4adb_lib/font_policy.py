"""Shared host-side validation for runtime font basenames."""


def font_filename_error(filename: str) -> str:
    if not filename:
        return "empty_filename"
    if filename.startswith("."):
        return "hidden_filename"
    if "/" in filename or "\\" in filename or ".." in filename:
        return "path_traversal"
    suffix = filename.rsplit(".", 1)[-1].lower() if "." in filename else ""
    if suffix not in {"ttf", "ttc", "otf", "otc"}:
        return "unsupported_font"
    return ""
