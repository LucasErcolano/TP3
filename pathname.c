#include "pathname.h"
#include "directory.h"
#include "inode.h"
#include "unixfilesystem.h" // Para ROOT_INUMBER y struct inode (aunque inode.h la trae)
#include "filsys.h"       // Para struct filsys, si es necesario directamente (usualmente no en pathname)
#include "direntv6.h"     // Para struct direntv6
#include <stdio.h>
#include <string.h>
#include <assert.h> // assert no se usa activamente en esta implementación pero es común en el proyecto

// Constante para el valor de retorno en caso de error, según la especificación.
#define PATHNAME_LOOKUP_FAILURE -1
// Definir una longitud máxima para las rutas, para la copia local.
#define MAX_PATHNAME_LEN 256

/**
 * Returns the inode number associated with the specified pathname. This need only
 * handle absolute paths. Returns a negative number (-1 is fine) if an error is
 * encountered.
 */
int pathname_lookup(struct unixfilesystem *fs, const char *pathname) {
    // Validar la ruta de entrada
    if (pathname == NULL || pathname[0] == '\0') {
        fprintf(stderr, "Error pathname_lookup: Pathname is NULL or empty.\n");
        return PATHNAME_LOOKUP_FAILURE;
    }
    if (pathname[0] != '/') {
        fprintf(stderr, "Error pathname_lookup: Pathname '%s' is not an absolute path.\n", pathname);
        return PATHNAME_LOOKUP_FAILURE;
    }

    // Hacer una copia mutable de la ruta para strtok
    char path_copy[MAX_PATHNAME_LEN];
    if (strlen(pathname) + 1 > MAX_PATHNAME_LEN) {
        fprintf(stderr, "Error pathname_lookup: Pathname '%s' exceeds maximum length %d.\n", pathname, MAX_PATHNAME_LEN - 1);
        return PATHNAME_LOOKUP_FAILURE;
    }
    strcpy(path_copy, pathname);

    // Caso especial: la ruta es "/"
    if (strcmp(path_copy, "/") == 0) {
        return ROOT_INUMBER; // El inodo del directorio raíz
    }

    // Iniciar el recorrido desde el directorio raíz.
    // current_dir_inumber siempre será el inodo del directorio en el que estamos buscando el siguiente componente.
    int current_dir_inumber = ROOT_INUMBER;
    struct inode dir_inode_obj; // Para verificar si current_dir_inumber es un directorio

    // Tokenizar la ruta. path_copy no debe ser "/" aquí.
    // strtok modificará path_copy. El primer token no será vacío porque path_copy[0] era '/'.
    char *component = strtok(path_copy, "/");

    while (component != NULL) {
        // Verificar que el inodo del directorio actual (current_dir_inumber) es realmente un directorio.
        if (inode_iget(fs, current_dir_inumber, &dir_inode_obj) < 0) {
            // Error al obtener el inodo del directorio actual. inode_iget ya imprimió un error.
            return PATHNAME_LOOKUP_FAILURE;
        }

        if ((dir_inode_obj.i_mode & IFMT) != IFDIR) {
            fprintf(stderr, "Error pathname_lookup: Path component (inode %d) is not a directory while resolving '%s'.\n", current_dir_inumber, pathname);
            return PATHNAME_LOOKUP_FAILURE;
        }

        // Buscar el componente actual dentro del directorio actual (current_dir_inumber)
        struct direntv6 found_entry;
        if (directory_findname(fs, component, current_dir_inumber, &found_entry) < 0) {
            // Componente no encontrado. directory_findname podría haber impreso un error.
            fprintf(stderr, "Error pathname_lookup: Component '%s' not found in directory (inode %d) while resolving '%s'.\n", component, current_dir_inumber, pathname);
            return PATHNAME_LOOKUP_FAILURE;
        }

        // Componente encontrado. El inodo de este componente se convierte en el
        // "directorio actual" para la siguiente iteración (si hay más componentes)
        // o será el resultado final.
        current_dir_inumber = found_entry.d_inumber;

        // Obtener el siguiente componente
        component = strtok(NULL, "/");
    }

    // Si el bucle termina, current_dir_inumber contiene el número de inodo
    // del último componente de la ruta.
    return current_dir_inumber;
}
