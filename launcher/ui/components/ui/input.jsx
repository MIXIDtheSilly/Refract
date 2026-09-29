import * as React from "react"
import { cn } from "@/lib/utils"

function Input({
  className,
  type,
  ...props
}) {
  return (
    <input
      type={type}
      data-slot="input"
      className={cn(
        "h-9 w-full min-w-0 rounded-lg border border-input bg-black/15 px-3 py-1 text-sm transition-[border-color,background-color] outline-none placeholder:text-muted-foreground hover:border-[#5a5a5a] disabled:pointer-events-none disabled:opacity-50",
        "focus-visible:border-foreground/70 focus-visible:bg-black/25 focus-visible:outline-none",
        "aria-invalid:border-destructive",
        className
      )}
      {...props} />
  );
}

export { Input }
