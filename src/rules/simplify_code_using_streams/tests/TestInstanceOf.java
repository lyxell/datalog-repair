import java.util.ArrayList;
import java.util.List;

public class TestInstanceOf {
    public static void main(String[] args) {
        ArrayList<Integer> original = new ArrayList<>();
        original.add(1);
        original.add(2);
        original.add(3);
        original.add(4);
        List<Integer> result = new ArrayList<>();
        for (Integer x : original) {
            if (x < 2) {
                result.add(x + 3);
            }
        }
        System.out.println(result);
    }
}
