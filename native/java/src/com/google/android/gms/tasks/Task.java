package com.google.android.gms.tasks;

public abstract class Task<TResult> {
    public boolean isComplete() { return true; }
    public boolean isSuccessful() { return false; }
    public boolean isCanceled() { return false; }
    public TResult getResult() { return null; }
    public Exception getException() { return null; }
    public Task<TResult> addOnSuccessListener(OnSuccessListener<? super TResult> listener) { return this; }
    public Task<TResult> addOnFailureListener(OnFailureListener listener) { return this; }
    public Task<TResult> addOnCompleteListener(OnCompleteListener<TResult> listener) { return this; }
}
